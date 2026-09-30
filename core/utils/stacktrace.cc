// SPDX-License-Identifier: BSD-3-Clause

#include "stacktrace.h"

#include <dlfcn.h>
#include <execinfo.h>
#include <limits.h>
#include <link.h>

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include "format.h"

namespace bess::utils {

static std::string FetchLine(std::string filename, int lineno, int context) {
  std::ostringstream ret;
  std::string line;

  std::ifstream f(filename);
  if (!f) {
    return "        (file/line not available)\n";
  }

  for (int curr = 1; std::getline(f, line) && curr <= lineno + context;
       curr++) {
    if (std::abs(curr - lineno) <= context) {
      ret << "      " << (curr == lineno ? "->" : "  ") << " " << curr << ": "
          << line << std::endl;
    }
  }

  return ret.str();
}

// Run an external command and return its standard output.
static std::string RunCommand(std::string cmd) {
  std::ostringstream ret;

  FILE *proc = popen(cmd.c_str(), "r");
  if (proc) {
    while (!feof(proc)) {
      char buf[PIPE_BUF];
      size_t bytes = fread(buf, 1, sizeof(buf), proc);
      ret.write(buf, bytes);
    }
    pclose(proc);
  }

  return ret.str();
}

// If mmap is used (as for shared objects), code address at runtime can be
// arbitrary. This function translates an absolute address into a relative
// address to the object file it belongs to, based on the current memory
// mapping of this process.
static uintptr_t GetRelativeAddress(uintptr_t abs_addr) {
  Dl_info info;
  struct link_map *map;

  if (dladdr1(reinterpret_cast<void *>(abs_addr), &info,
              reinterpret_cast<void **>(&map), RTLD_DL_LINKMAP) != 0) {
    // Normally the base_addr of the executable will be just 0x0
    uintptr_t base_addr = reinterpret_cast<uintptr_t>(map->l_addr);
    return abs_addr - base_addr;
  } else {
    // Error happend. Use the absolute address as a fallback, hoping the address
    // is from the main executable.
    return abs_addr;
  }
}

// addr2line must be available.
// Returns the code lines [lineno - context, lineno + context]
static std::string PrintCode(std::string symbol, int context) {
  std::ostringstream ret;
  char objfile[PATH_MAX];
  char addr[1024];

  // Symbol examples:
  // ./bessd(run_worker+0x8e) [0x419d0e]
  // ./bessd() [0x4149d8]
  // /home/foo/.../source.so(_ZN6Source7RunTaskEPv+0x55) [0x7f09e912c7b5]
  Parse(symbol, "%[^(](%*s [%[^]]]", objfile, addr);

  uintptr_t sym_addr = std::strtoull(addr, nullptr, 16);
  uintptr_t obj_addr = GetRelativeAddress(sym_addr);

  std::string cmd =
      Format("addr2line -C -i -f -p -e %s 0x%" PRIxPTR " 2> /dev/null", objfile,
             obj_addr);

  std::istringstream result(RunCommand(cmd));
  std::string line;

  while (std::getline(result, line)) {
    // addr2line examples:
    // sched_free at /home/sangjin/.../tc.c:277 (discriminator 2)
    // run_worker at /home/sangjin/bess/core/module.c:653

    ret << "    " << line << std::endl;

    auto pos = line.find(" at ");
    if (pos == std::string::npos) {
      // failed to parse the line
      continue;
    }

    // Remove unnecessary characters (up to " at ", including itself)
    line.erase(0, pos + 4);

    char filename[PATH_MAX];
    int lineno;

    if (Parse(line, "%[^:]:%d", filename, &lineno) == 2) {
      if (std::string(filename) != "??" && lineno != 0) {
        ret << FetchLine(filename, lineno, context);
      }
    }
  }

  return ret.str();
}

static bool SkipSymbol(char *symbol) {
  static const char *blacklist[] = {"(_ZN6google10LogMessage",
                                    "(_ZN6google15LogMessageFatal"};

  for (auto prefix : blacklist) {
    if (std::strstr(symbol, prefix)) {
      return true;
    }
  }

  return false;
}

[[gnu::noinline]] std::string StackTrace(void *trap_ip) {
  const size_t max_stack_depth = 64;
  void *addrs[max_stack_depth] = {};

  std::ostringstream stack;

  char **symbols;
  int skips = 0;

  // the linker requires -rdynamic for non-exported symbols
  int cnt = backtrace(addrs, max_stack_depth);

  // in some cases the bottom of the stack is NULL - not useful at all, remove.
  while (cnt > 0 && !addrs[cnt - 1]) {
    cnt--;
  }

  // The return addresses point to the next instruction after their call,
  // so adjust them by -1
  for (int i = 0; i < cnt; i++) {
    if (addrs[i] != trap_ip) {
      addrs[i] =
          reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(addrs[i]) - 1);
    }
  }

  symbols = backtrace_symbols(addrs, cnt);
  if (!symbols) {
    return "ERROR: backtrace_symbols() failed\n";
  }

  // Triggered by a signal? (trap_ip is set)
  if (trap_ip) {
    // addrs[0]: StackTrace() <- this function calling backtrace()
    // addrs[1]: TrapHandler()
    // addrs[2]: sigaction in glibc, or pthread signal handler
    // addrs[3]: the triggering instruction pointer or its caller
    //           (depending on the kernel behavior?)
    if (addrs[3] == trap_ip) {
      skips = 3;
    } else {
      skips = 2;
      addrs[2] = trap_ip;
    }
  } else {
    // LOG(FATAL) or CHECK() failed
    // addrs[0]: StackTrace() <- this function calling backtrace()
    // addrs[1]: caller
    // addrs[2]: caller's caller
    skips = 2;
    while (skips < cnt && SkipSymbol(symbols[skips])) {
      skips++;
    }
  }

  stack << "Backtrace (recent calls first) ---" << std::endl;

  for (int i = skips; i < cnt; i++) {
    stack << "(" << i - skips << "): " << symbols[i] << std::endl;
    stack << PrintCode(symbols[i], (i == skips) ? 3 : 0);
  }

  free(symbols);  // required by backtrace_symbols()

  return stack.str();
}

}  // namespace bess::utils
