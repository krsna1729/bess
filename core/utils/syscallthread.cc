// Copyright (c) 2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include <cstring>

#include "syscallthread.h"

#include "utils/logging.h"

namespace bess {
namespace utils {

constexpr int SIG_THREAD_EXIT = SIGUSR2;

// We only catch the exit signal once, process wide, so we want
// a flag to tell if we've done that.  Likewise, the per-thread
// signal mask is the same across all such threads, so we only
// need one instance.
struct ExitSigMask {
  bool initialized;
  sigset_t allmask;
  sigset_t mostmask;
};

static struct ExitSigMask ProcessMasks = {
    .initialized = false, .allmask = {}, .mostmask = {}};

static inline const sigset_t *GetMask(bool all) {
  assert(ProcessMasks.initialized);
  return all ? &ProcessMasks.allmask : &ProcessMasks.mostmask;
}

/*
 * When we receive an exit signal, that will interrupt any
 * in-progress system call if appropriate.  We just need a
 * no-op signal handler that the system will call, interrupting
 * the system-call-in-progress.
 */
void ExitRequestHandler(int) {}

/*
 * Establishes the exit signal masks and handlers.
 */
bool CatchExitSignal() {
  if (!ProcessMasks.initialized) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = ExitRequestHandler;
    if (sigaction(SIG_THREAD_EXIT, &sa, NULL) < 0) {
      return false;
    }
    sigfillset(&ProcessMasks.allmask);   // complete blockage of everything
    sigfillset(&ProcessMasks.mostmask);  // block all except SIG_THREAD_EXIT
    sigdelset(&ProcessMasks.mostmask, SIG_THREAD_EXIT);

    ProcessMasks.initialized = true;
  }
  return true;
}

// pthread_sigmask returns an error number (never -1 with errno); with a
// valid mask it cannot fail.
static inline void PthreadSetSigmask(const sigset_t *mask) {
  const int rc = pthread_sigmask(SIG_SETMASK, mask, nullptr);
  CHECK_EQ(rc, 0) << "pthread_sigmask: " << strerror(rc);
}

/*
 * Run a thread that (among whatever else it does) makes system calls,
 * then checks for IsExitRequested() after making any blocking system
 * calls.
 *
 * We first set it up with a per-thread signal mask that blocks all
 * but SIG_THREAD_EXIT, or if the only blocking system call it makes
 * is pselect()/ppoll(), blocks all signals entirely.
 */
void SyscallThread::RunInThread(SyscallThread *syscaller, bool reliable) {
  // Block the appropriate set of signals.
  PthreadSetSigmask(GetMask(reliable));

  // Note that we're now ready to act upon SIG_THREAD_EXIT, i.e.,
  // we have the right signal mask established.  This is really
  // just for debug - we have to be able to act on it early.
  syscaller->SetState(SyscallThread::ThreadState::kReady);

  // Run the user's code.  Note that it's possible that we were told
  // to exit already, e.g., before we finished setting the signal mask;
  // in this case, do NOT run the user's code (it might block forever,
  // if pfuncs is true).
  if (!syscaller->IsExitRequested()) {
    syscaller->Run();
  }

  // We're done; remark on this and terminate (by returning).
  syscaller->SetState(SyscallThread::ThreadState::kDone);
}

/*
 * Send the "please exit" signal that interrupts a system call
 * to the indicated thread.  In other words, give it a kick to
 * get it out of system calls.
 */
void SyscallThread::KickThread() {
  pthread_kill(thread_.native_handle(), SIG_THREAD_EXIT);
}

// Get the signal mask for a pselect() or ppoll() system call.
const sigset_t *SyscallThreadPfuncs::Sigmask() const {
  return GetMask(false);
}

/*
 * Blocks most (all==false) or all (all==true) signals.  Called from
 * the {Push,Pop}Defer methods of SyscallThreadAny.
 */
void SyscallThreadAny::BlockSignals(bool all) {
  PthreadSetSigmask(GetMask(all));
}

/*
 * Runs the knock thread: keeps kicking the normal thread until
 * it acknowledges that it is exiting or has exited.
 */
void SyscallThreadAny::Knocker(SyscallThreadAny *syscaller) {
  while (!syscaller->ExitingOrExited()) {
    syscaller->KickThread();
    // Use nanosleep so that SIG_THREAD_EXIT will interrupt us.
    // The documentation says that sleep_for waits for the full
    // time (presumably using a loop with nanosleep calls).
    //
    // std::this_thread::sleep_for(std::chrono::milliseconds(250));
    struct timespec delay = {.tv_sec = 0, .tv_nsec = 250 * 1000 * 1000};
    nanosleep(&delay, nullptr);
  }
}

/*
 * Waits for the knock thread to exit.
 *
 * Note that WaitForKnockThread is called after the regular thread
 * is join()ed, so by definition the knock thread doesn't need to
 * run any more.
 */
void SyscallThreadAny::WaitForKnockThread() {
  if (knock_thread_.joinable()) {
    // Kick the knock thread now to make it finish early if it's
    // stuck in nanosleep (see syscallthread.cc).
    pthread_kill(knock_thread_.native_handle(), SIG_THREAD_EXIT);
    knock_thread_.join();
  }
}

}  // namespace bess
}  // namespace utils
