// Copyright (c) 2014-2016, The Regents of the University of California.
// Copyright (c) 2016-2017, Nefeli Networks, Inc.
// SPDX-License-Identifier: BSD-3-Clause

#include "debug.h"

#include <cxxabi.h>
#include <gnu/libc-version.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

#include "utils/logging.h"
#include <gflags/gflags.h>
#include <rte_config.h>
#include <rte_version.h>

#include <cassert>
#include <csignal>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "module.h"
#include "packet.h"
#include "scheduler.h"
#include "traffic_class.h"
#include "runtime/opts.h"
#include "utils/format.h"
#include "utils/stacktrace.h"

namespace bess {
namespace debug {

using bess::utils::Format;

static const char *si_code_to_str(int sig_num, int si_code) {
  /* See the manpage of sigaction() */
  switch (si_code) {
    case SI_USER:
      return "SI_USER: kill";
    case SI_KERNEL:
      return "SI_KERNEL: sent by the kernel";
    case SI_QUEUE:
      return "SI_QUEUE: sigqueue";
    case SI_TIMER:
      return "SI_TIMER: POSIX timer expired";
    case SI_MESGQ:
      return "SI_MESGQ: POSIX message queue state changed";
    case SI_ASYNCIO:
      return "SI_ASYNCIO: AIO completed";
    case SI_SIGIO:
      return "SI_SIGIO: Queued SIGIO";
    case SI_TKILL:
      return "SI_TKILL: tkill or tgkill";
  }

  switch (sig_num) {
    case SIGILL:
      switch (si_code) {
        case ILL_ILLOPC:
          return "ILL_ILLOPC: illegal opcode";
        case ILL_ILLOPN:
          return "ILL_ILLOPN: illegal operand";
        case ILL_ILLADR:
          return "ILL_ILLADR: illegal addressing mode";
        case ILL_ILLTRP:
          return "ILL_ILLTRP: illegal trap";
        case ILL_PRVOPC:
          return "ILL_PRVOPC: privileged opcode";
        case ILL_PRVREG:
          return "ILL_PRVREG: privileged register";
        case ILL_COPROC:
          return "ILL_COPROC: coprocessor error";
        case ILL_BADSTK:
          return "ILL_PRVREG: internal stack error";
        default:
          return "unknown";
      }

    case SIGFPE:
      switch (si_code) {
        case FPE_INTDIV:
          return "FPE_INTDIV: integer divide by zero";
        case FPE_INTOVF:
          return "FPE_INTOVF: integer overflow";
        case FPE_FLTDIV:
          return "FPE_FLTDIV: floating-point divide by zero";
        case FPE_FLTOVF:
          return "FPE_FLTOVF: floating-point overflow";
        case FPE_FLTUND:
          return "FPE_FLTOVF: floating-point underflow";
        case FPE_FLTRES:
          return "FPE_FLTOVF: floating-point inexact result";
        case FPE_FLTINV:
          return "FPE_FLTOVF: floating-point invalid operation";
        case FPE_FLTSUB:
          return "FPE_FLTOVF: subscript out of range";
        default:
          return "unknown";
      }

    case SIGSEGV:
      switch (si_code) {
        case SEGV_MAPERR:
          return "SEGV_MAPERR: address not mapped to object";
        case SEGV_ACCERR:
          return "SEGV_ACCERR: invalid permissions for mapped object";
#if defined(SEGV_BNDERR)
        case SEGV_BNDERR:
          return "SEGV_BNDERR: failed address bound checks";
#endif
#if defined(SEGV_PKUERR)
        case SEGV_PKUERR:
          return "SEGV_PKUERR: failed protection key checks";
#endif
        default:
          return "unknown";
      }

    case SIGBUS:
      switch (si_code) {
        case BUS_ADRALN:
          return "BUS_ADRALN: invalid address alignment";
        case BUS_ADRERR:
          return "BUS_ADRERR: nonexistent physical address";
        case BUS_OBJERR:
          return "BUS_OBJERR: object-specific hardware error";
#if defined(BUS_MCEERR_AR)
        case BUS_MCEERR_AR:
          return "BUS_MCEERR_AR: Hardware memory error consumed on a machine "
                 "check";
#endif
#if defined(BUS_MCEERR_AO)
        case BUS_MCEERR_AO:
          return "BUS_MCEERR_AO: Hardware memory error detected in process but "
                 "not consumed";
#endif
        default:
          return "unknown";
      }
  }

  return "si_code unavailable for unknown signal";
}


static void *trap_ip;
static std::string oops_msg;


[[noreturn]] static void exit_failure() {
  _exit(EXIT_FAILURE);
}

[[noreturn]] static void abort_failure() {
  abort();
}

[[ gnu::noinline, noreturn ]] void GoPanic() {
  if (oops_msg == "")
    oops_msg = utils::StackTrace(trap_ip);

  // Create a crash log file if not disabled.
  if (!FLAGS_no_crashlog) {
    try {
      std::ofstream fp(P_tmpdir "/bessd_crash.log");
      fp << oops_msg;
      fp.close();
    } catch (...) {
      // Ignore any errors.
    }
  }

  if (FLAGS_core_dump) {
    // Set SIGABRT back to the default to avoid catching the abort used to
    // generate the core file.
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_DFL;
    sigaction(SIGABRT, &sa, 0);
    // Clang (unlike GCC) does not implicitly convert a function pointer
    // whose noreturn-ness comes from the C++11 [[noreturn]] attribute into
    // glog's logging_fail_func_t, which spells noreturn via the GNU
    // __attribute__ instead -- the two attribute spellings aren't
    // interchangeable for this conversion under Clang, which also rejects
    // static_cast between them (they aren't "related" function pointer
    // types in its view). reinterpret_cast is the correct tool here: both
    // sides are ordinary function pointers with identical calling
    // convention and signature, differing only in an attribute that isn't
    // part of the ABI.
    google::InstallFailureFunction(
        reinterpret_cast<google::logging_fail_func_t>(abort_failure));
  } else {
    google::InstallFailureFunction(
        reinterpret_cast<google::logging_fail_func_t>(exit_failure));
  }
  LOG(FATAL) << oops_msg;
}

// SIGUSR1 is used to examine the current callstack, without aborting.
// (useful when the process seems stuck)
// TODO: Only use async-signal-safe operations in the signal handler.
static void TrapHandler(int sig_num, siginfo_t *info, void *ucontext) {
  std::ostringstream oops;
  auto *uc = static_cast<ucontext_t *>(ucontext);
  bool is_fatal = (sig_num != SIGUSR1);
  static volatile bool already_trapped = false;

  // avoid recursive traps
  if (is_fatal && !__sync_bool_compare_and_swap(&already_trapped, false, true))
    return;

#if __i386
  trap_ip = reinterpret_cast<void *>(uc->uc_mcontext.gregs[REG_EIP]);
#elif __x86_64
  trap_ip = reinterpret_cast<void *>(uc->uc_mcontext.gregs[REG_RIP]);
#else
#error neither x86 or x86-64
#endif

  if (is_fatal) {
    oops << "A critical error has occured. Aborting..." << std::endl;
  }

  oops << "Signal: " << sig_num << " (" << strsignal(sig_num)
       << "), si_code: " << info->si_code << " ("
       << si_code_to_str(sig_num, info->si_code) << ")" << std::endl;

  oops << "pid: " << getpid() << ", tid: " << (pid_t)syscall(SYS_gettid)
       << ", address: " << info->si_addr << ", IP: " << trap_ip << std::endl;

  if (is_fatal) {
    oops << utils::StackTrace(trap_ip);
    oops_msg = oops.str();
    GoPanic();
    // Never reaches here. LOG(FATAL) will terminate the process.
  } else {
    LOG(INFO) << oops.str() << utils::StackTrace(trap_ip);
    trap_ip = nullptr;
  }
}

void SetTrapHandler() {
  const int signals[] = {
      SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT,
      // SIGUSR1 is special in that it is triggered by user and does not abort
      SIGUSR1,
  };

  const int ignored_signals[] = {
      SIGPIPE,
  };

  struct sigaction sigact;
  size_t i;

  unlink(P_tmpdir "/bessd_crash.log");

  sigact.sa_sigaction = TrapHandler;
  sigact.sa_flags = SA_RESTART | SA_SIGINFO;

  for (i = 0; i < sizeof(signals) / sizeof(int); i++) {
    int ret = sigaction(signals[i], &sigact, nullptr);
    DCHECK_NE(ret, 1);
  }

  for (i = 0; i < sizeof(ignored_signals) / sizeof(int); i++) {
    signal(ignored_signals[i], SIG_IGN);
  }
}

template <typename T>
static void DumpType() {
  std::string type_name = typeid(T).name();
  char *demangled;
  int ret;

  demangled = abi::__cxa_demangle(type_name.c_str(), nullptr, nullptr, &ret);
  if (ret == 0) {
    type_name = demangled;
    std::free(demangled);
  } else {
    DCHECK_EQ(ret, 0);
  }

  std::cout << Format("%-24s %8zu %8zu", type_name.c_str(), sizeof(T),
                      alignof(T))
            << std::endl;
}

void DumpTypes(void) {
  std::cout << "bessd " << google::VersionString() << std::endl;
  std::cout << Format("gcc %d.%d.%d", __GNUC__, __GNUC_MINOR__,
                      __GNUC_PATCHLEVEL__)
            << std::endl;
  std::cout << Format("glibc %s-%s", gnu_get_libc_version(),
                      gnu_get_libc_release())
            << std::endl;
#if defined(__GLIBCXX__)
  std::cout << "libstdc++ " << __GLIBCXX__ << std::endl;
#elif defined(__GLIBCPP__)
  std::cout << "libstdc++ " << __GLIBCPP__ << std::endl;
#else
  std::cout << "libstdc++ ?" << std::endl;
#endif
  std::cout << rte_version() << std::endl;

  std::cout << Format("%-24s %8s %8s", "", "sizeof", "alignof") << std::endl;

  // basic types
  DumpType<char>();
  DumpType<short>();
  DumpType<int>();
  DumpType<long>();
  DumpType<long long>();
  DumpType<intmax_t>();
  DumpType<void *>();
  DumpType<size_t>();
  DumpType<max_align_t>();

  // BESS types
  DumpType<rte_mbuf>();
  DumpType<bess::PacketRef>();
  DumpType<bess::PacketBatch>();

  DumpType<Scheduler>();
  DumpType<TrafficClass>();
  DumpType<Task>();

  DumpType<Module>();
  DumpType<bess::Gate>();
  DumpType<bess::IGate>();
  DumpType<bess::OGate>();

  DumpType<Worker>();
}

}  // namespace debug
}  // namespace bess
