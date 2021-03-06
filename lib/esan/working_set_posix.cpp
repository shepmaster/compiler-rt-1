//===-- working_set_posix.cpp -----------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file is a part of EfficiencySanitizer, a family of performance tuners.
//
// POSIX-specific working set tool code.
//===----------------------------------------------------------------------===//

#include "working_set.h"
#include "esan_flags.h"
#include "esan_shadow.h"
#include "sanitizer_common/sanitizer_common.h"
#include "sanitizer_common/sanitizer_linux.h"
#include <signal.h>
#include <sys/mman.h>

namespace __esan {

// We only support regular POSIX threads with a single signal handler
// for the whole process == thread group.
// Thus we only need to store one app signal handler.
// FIXME: Store and use any alternate stack and signal flags set by
// the app.  For now we just call the app handler from our handler.
static __sanitizer_sigaction AppSigAct;

bool processWorkingSetSignal(int SigNum, void (*Handler)(int),
                             void (**Result)(int)) {
  VPrintf(2, "%s: %d\n", __FUNCTION__, SigNum);
  if (SigNum == SIGSEGV) {
    *Result = AppSigAct.handler;
    AppSigAct.sigaction = (void (*)(int, void*, void*))Handler;
    return false; // Skip real call.
  }
  return true;
}

bool processWorkingSetSigaction(int SigNum, const void *ActVoid,
                                void *OldActVoid) {
  VPrintf(2, "%s: %d\n", __FUNCTION__, SigNum);
  if (SigNum == SIGSEGV) {
    const struct sigaction *Act = (const struct sigaction *) ActVoid;
    struct sigaction *OldAct = (struct sigaction *) OldActVoid;
    if (OldAct)
      internal_memcpy(OldAct, &AppSigAct, sizeof(OldAct));
    if (Act)
      internal_memcpy(&AppSigAct, Act, sizeof(AppSigAct));
    return false; // Skip real call.
  }
  return true;
}

static void reinstateDefaultHandler(int SigNum) {
  __sanitizer_sigaction SigAct;
  internal_memset(&SigAct, 0, sizeof(SigAct));
  SigAct.sigaction = (void (*)(int, void*, void*)) SIG_DFL;
  int Res = internal_sigaction(SigNum, &SigAct, nullptr);
  CHECK(Res == 0);
  VPrintf(1, "Unregistered for %d handler\n", SigNum);
}

// If this is a shadow fault, we handle it here; otherwise, we pass it to the
// app to handle it just as the app would do without our tool in place.
static void handleMemoryFault(int SigNum, void *Info, void *Ctx) {
  if (SigNum == SIGSEGV) {
    // We rely on si_addr being filled in (thus we do not support old kernels).
    siginfo_t *SigInfo = (siginfo_t *)Info;
    uptr Addr = (uptr)SigInfo->si_addr;
    if (isShadowMem(Addr)) {
      VPrintf(3, "Shadow fault @%p\n", Addr);
      uptr PageSize = GetPageSizeCached();
      int Res = internal_mprotect((void *)RoundDownTo(Addr, PageSize),
                                  PageSize, PROT_READ|PROT_WRITE);
      CHECK(Res == 0);
    } else if (AppSigAct.sigaction) {
      // FIXME: For simplicity we ignore app options including its signal stack
      // (we just use ours) and all the delivery flags.
      AppSigAct.sigaction(SigNum, Info, Ctx);
    } else {
      // Crash instead of spinning with infinite faults.
      reinstateDefaultHandler(SigNum);
    }
  } else
    UNREACHABLE("signal not registered");
}

void registerMemoryFaultHandler() {
  // We do not use an alternate signal stack, as doing so would require
  // setting it up for each app thread.
  // FIXME: This could result in problems with emulating the app's signal
  // handling if the app relies on an alternate stack for SIGSEGV.

  // We assume SIGSEGV is not blocked and won't be blocked by the app, so
  // we leave the mask alone.

  __sanitizer_sigaction SigAct;
  internal_memset(&SigAct, 0, sizeof(SigAct));
  SigAct.sigaction = handleMemoryFault;
  // We want to handle nested signals b/c we need to handle a
  // shadow fault in an app signal handler.
  SigAct.sa_flags = SA_SIGINFO | SA_NODEFER;
  int Res = internal_sigaction(SIGSEGV, &SigAct, &AppSigAct);
  CHECK(Res == 0);
  VPrintf(1, "Registered for SIGSEGV handler\n");
}

} // namespace __esan
