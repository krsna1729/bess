#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Architecture boundary check (M21, D-071, D-190).

ISA code lives in two kinds of place: core/arch/ (the platform layer and the
primitives with several unrelated users) and owner kernel files, named
`<owner>_kernels.h` or `<owner>_kernels.cc`, next to the one module or library
that uses them (D-190). Everywhere else under core/, sample_plugin/ and
examples/ (tests and benchmarks included: CI builds and runs them on arm64
too), this counts four kinds of finding per file:

  include    an #include of an intrinsic or CPU-specific header
             (<x86intrin.h>, <immintrin.h>, <arm_neon.h>, <cpuid.h>,
             <rte_vect.h>, ...)
  condition  a preprocessor condition (#if/#ifdef/#ifndef/#elif) that names an
             architecture or ISA macro (__x86_64__, __aarch64__, __SSE4_2__,
             __AVX2__, __ARM_*, RTE_ARCH_*, BESS_ARCH_*, ...) or an intrinsic
             header (__has_include(<immintrin.h>))
  asm        an asm statement with an instruction in it. The empty-template
             compiler barrier `asm volatile("" ::: "memory")` and an
             `__asm__("symbol")` declaration label are portable and not counted
  intrinsic  a line that uses x86 or Arm intrinsics, vector types, ISA builtins,
             architecture-only DPDK functions or CPU dispatch (_mm_*, __m128i,
             __rdtsc, __builtin_ia32_*, uint8x16_t, __crc32*, crc32c_sse42_*,
             __builtin_cpu_supports, __attribute__((target("..."))))

and compares the counts with tools/arch_allowlist.json, which grandfathers the
code that predates the boundary. Any difference fails:
  * a count above the file's entry, or a finding in a file without an entry:
    new architecture code belongs in core/arch/ or in the owner's kernel file;
  * a count below the entry, or an entry for a file with no finding: the
    allowlist is stale; lower the entry (or delete it) in the same change, so
    the allowlist only ever shrinks.

An owner kernel file outside core/arch/ may hold any of the four, under three
rules that keep cpu.h the one source of CPU-feature truth and keep a portable
path in every kernel:
  * its preprocessor conditions name ISA features only through cpu.h's
    BESS_ARCH_* macros: a condition naming a raw compiler macro (__AVX2__,
    __SSE4_2__, __x86_64__, __aarch64__, __ARM_*, ...), a DPDK one
    (RTE_ARCH_*, RTE_MACHINE_CPUFLAG_*, RTE_CPUFLAG_*) or
    __has_include(<immintrin.h>) fails. core/arch/ is exempt: cpu.h derives the
    BESS_ARCH_* macros from the raw ones. Only conditions are read, so a
    #define that aliases a raw macro is not caught; review covers that;
  * every ISA use (intrinsic-header include, intrinsic, asm) is guarded: some
    enclosing #if, #ifdef or #elif branch has a guard condition. A guard
    condition is `#ifdef M`, or an #if/#elif condition that, with comments
    removed and each `defined(N)` written `defined N`, has no parenthesis,
    `||`, `?` or `,` and is a chain of `&&` terms, one of which is exactly `M`
    or `defined M`. M is a BESS_ARCH_* feature macro other than
    BESS_ARCH_GENERIC (BESS_ARCH_X86, BESS_ARCH_ARM64, BESS_ARCH_AVX2, ...);
    the other terms may be anything. Every C operator except `||`, `?:` and
    `,` binds tighter than `&&`, so such a condition is false when M is
    undefined, and cpu.h leaves every M undefined in a BESS_ARCH_GENERIC
    build, so that build compiles the use out. An #else or #ifndef branch and
    any other condition (`!defined M`, `defined M == 0`, `defined(M) | 1`,
    `||`, an unrelated macro such as NDEBUG) guard nothing; a false positive
    only fails this check;
  * a kernel file with any ISA code has an alternative branch: an `#else`
    directive or a condition that names BESS_ARCH_GENERIC.
These prove that the ISA code is compiled out and that some other branch
exists, not that the other branch is right: the -Darch_generic and arm64
builds and the kernel's identity test prove that.

The allowlist is edited by hand and reviewed; this tool never writes it, and
it never lists core/arch/ or a kernel file. `--report` prints the current counts
in the allowlist's format (reasons kept, new entries marked) for that review.
Comments are ignored; string literals are kept (asm templates live in them).

Usage:
  tools/check_arch.py [--root DIR] [--allowlist FILE] [--verbose]
  tools/check_arch.py --report
  tools/check_arch.py --self-test
"""

import argparse
import bisect
import json
from pathlib import Path
import re
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]

SCAN_DIRS = ("core", "sample_plugin", "examples")
ARCH_DIR = "core/arch/"
# Owner kernel files (D-190): `<owner>_kernels.h` / `<owner>_kernels.cc`.
KERNEL_FILE = re.compile(r"(?:^|/)[^/]+_kernels\.(?:h|cc)$")
SOURCE_SUFFIXES = {".h", ".hh", ".hpp", ".c", ".cc", ".cpp", ".cxx", ".inc", ".ipp"}
CATEGORIES = ("include", "condition", "asm", "intrinsic")

# Headers that exist only for one architecture or expose its vector types.
INTRINSIC_HEADER = re.compile(
    r"^(?:\w*intrin\.h|arm_\w+\.h|cpuid\.h|rte_vect\.h|sys/platform/x86\.h)$"
)
INCLUDE_LINE = re.compile(r'^\s*#\s*include\s*[<"]([^">]+)[">]')
CONDITION_LINE = re.compile(r"^\s*#\s*(?:if|ifdef|ifndef|elif|elifdef|elifndef)\b")
ARCH_MACRO = re.compile(
    r"\b(?:__x86_64__|__x86_64|__amd64__|__amd64|__i386__|__i386|__i686__"
    r"|_M_X64|_M_AMD64|_M_IX86|_M_ARM64|_M_ARM"
    r"|__aarch64__|__arm64__|__arm__|__thumb__|__ARM_\w+"
    r"|__SSE\w*|__AVX\w*|__BMI\w*|__POPCNT__|__PCLMUL__|__LZCNT__|__FMA__"
    r"|__F16C__|__RTM__|__AES__|__SHA__|__MOVBE__|__VPCLMULQDQ__"
    r"|__riscv\w*|__powerpc\w*|__PPC\w*|__s390x__"
    r"|RTE_ARCH_\w+|RTE_MACHINE_CPUFLAG_\w+|RTE_CPUFLAG_\w+|BESS_ARCH_\w+)\b"
)
HAS_INCLUDE_INTRINSIC = re.compile(
    r"__has_include\s*\(\s*[<\"](?:\w*intrin\.h|arm_\w+\.h|cpuid\.h|rte_vect\.h)[>\"]"
)
INTRINSIC_USE = re.compile(
    r"\b(?:_mm\d*_\w+|__m(?:64|128|256|512)[a-z]*|__rdtscp?|_rdtsc"
    r"|__builtin_ia32_\w+|__builtin_aarch64_\w+|__builtin_arm_\w+"
    r"|__builtin_cpu_(?:supports|is|init)|target_clones"
    r"|__crc32\w*|__yield|__wfe|__sev|_xbegin|_xend|_xabort|_xtest|__cpuid\w*"
    r"|(?:u?int|float|poly|bfloat)\d+x\d+(?:x\d)?_t|svbool_t|sv(?:u?int|float)\d+_t"
    r"|crc32c_sse42_\w+|crc32c_arm64_\w+)\b"
    r"|\btarget\s*\(\s*\""
)
ASM_KEYWORD = re.compile(r"\b(?:asm|__asm__|__asm)\b")
ELSE_LINE = re.compile(r"^\s*#\s*else\b")
GENERIC_MACRO = re.compile(r"\bBESS_ARCH_GENERIC\b")
DIRECTIVE = re.compile(r"^\s*#\s*(\w+)\s*(.*)$", re.S)
# A guard condition's shape (see _guards): `defined(N)` flattened to
# `defined N`, then one `&&` term that is exactly `M` or `defined M`.
DEFINED_CALL = re.compile(r"defined\s*\(\s*(\w+)\s*\)")
GUARD_TERM = re.compile(r"(?:defined\s+)?(BESS_ARCH_\w+)")
ASM_QUALIFIERS = re.compile(r"\s*(?:volatile|__volatile__|__volatile|inline|goto)\b")
SYMBOL_NAME = re.compile(r"^[A-Za-z_.$][\w.$]*$")


def strip_comments(text):
    """`text` with every comment blanked (newlines kept, so line numbers hold).

    String and character literals are kept verbatim; a comment marker inside
    one is not a comment.
    """
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if c == "/" and nxt == "/":
            while i < n and text[i] != "\n":
                # a backslash-newline continues a // comment
                if text[i] == "\\" and i + 1 < n and text[i + 1] == "\n":
                    out.append(" \n")
                    i += 2
                    continue
                out.append(" ")
                i += 1
        elif c == "/" and nxt == "*":
            end = text.find("*/", i + 2)
            end = n if end < 0 else end + 2
            out.append("".join("\n" if ch == "\n" else " " for ch in text[i:end]))
            i = end
        elif c == "R" and nxt == '"' and (i == 0 or not (text[i - 1].isalnum() or text[i - 1] == "_")):
            m = re.match(r'R"([^()\\\s]{0,16})\(', text[i:])
            if not m:
                out.append(c)
                i += 1
                continue
            close = ")" + m.group(1) + '"'
            end = text.find(close, i + m.end())
            end = n if end < 0 else end + len(close)
            out.append(text[i:end])
            i = end
        elif c == '"' or (c == "'" and not (i > 0 and text[i - 1].isalnum())):
            # (a ' after a digit or letter is a digit separator: 1'000)
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            out.append(text[i:j])
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def logical_directives(lines):
    """(line number, text) of each preprocessor directive, continuations joined."""
    i = 0
    while i < len(lines):
        line = lines[i]
        start = i
        if line.lstrip().startswith("#"):
            joined = line
            while joined.endswith("\\") and i + 1 < len(lines):
                i += 1
                joined = joined[:-1] + " " + lines[i]
            yield start + 1, joined
        i += 1


def string_literals(fragment):
    return [m.group(1) for m in re.finditer(r'"((?:[^"\\]|\\.)*)"', fragment)]


def asm_statements(text):
    """Line numbers of asm statements that contain an instruction."""
    found = []
    for m in ASM_KEYWORD.finditer(text):
        pos = m.end()
        qualified = False
        while True:
            q = ASM_QUALIFIERS.match(text, pos)
            if not q:
                break
            qualified = True
            pos = q.end()
        while pos < len(text) and text[pos].isspace():
            pos += 1
        if pos >= len(text) or text[pos] != "(":
            continue  # `asm` as a word, not a statement
        # the operand list, up to the matching parenthesis
        depth, j, in_str = 0, pos, False
        while j < len(text):
            ch = text[j]
            if in_str:
                if ch == "\\":
                    j += 1
                elif ch == '"':
                    in_str = False
            elif ch == '"':
                in_str = True
            elif ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
                if depth == 0:
                    break
            j += 1
        body = text[pos + 1:j]
        template_part = body.split(":", 1)[0] if ":" in _outside_strings(body) else body
        template = "".join(string_literals(template_part)).strip()
        if not template:
            continue  # compiler barrier: asm volatile("" ::: "memory")
        before = text[:m.start()].rstrip()
        prev = before[-1:] if before else ""
        if (not qualified and ":" not in _outside_strings(body)
                and SYMBOL_NAME.match(template)
                and (prev == ")" or prev == "]" or prev.isalnum() or prev == "_")):
            continue  # declaration label: int f() __asm__("symbol");
        found.append(text.count("\n", 0, m.start()) + 1)
    return found


def _outside_strings(fragment):
    return re.sub(r'"(?:[^"\\]|\\.)*"', '""', fragment)


def scan_text(text):
    """{category: [line numbers]} of one file's findings."""
    clean = strip_comments(text)
    lines = clean.split("\n")
    found = {c: [] for c in CATEGORIES}
    for line_no, directive in logical_directives(lines):
        inc = INCLUDE_LINE.match(directive)
        if inc:
            if INTRINSIC_HEADER.match(inc.group(1).strip()):
                found["include"].append(line_no)
            continue
        if CONDITION_LINE.match(directive) and (
                ARCH_MACRO.search(directive) or HAS_INCLUDE_INTRINSIC.search(directive)):
            found["condition"].append(line_no)
    for line_no, line in enumerate(lines, 1):
        if INTRINSIC_USE.search(_outside_strings(line)):
            found["intrinsic"].append(line_no)
    found["asm"] = asm_statements(clean)
    return {c: v for c, v in found.items() if v}


def is_scanned(rel):
    return (rel.split("/", 1)[0] in SCAN_DIRS and not rel.startswith(ARCH_DIR)
            and Path(rel).suffix in SOURCE_SUFFIXES)


def is_kernel_file(rel):
    """An owner kernel file (D-190): ISA code allowed, under kernel_rules()."""
    return is_scanned(rel) and KERNEL_FILE.search(rel) is not None


def _guards(condition):
    """Whether a branch under `condition` (comments already blanked) is
    compiled out without an ISA feature macro: with each `defined(N)` written
    `defined N`, it has no parenthesis, `||`, `?` or `,`, and one of its `&&`
    terms is exactly `M` or `defined M` for a BESS_ARCH_* macro M other than
    BESS_ARCH_GENERIC. Every C operator except `||`, `?:` and `,` binds
    tighter than `&&`, so the condition is false when M is undefined."""
    flat = DEFINED_CALL.sub(r"defined \1", condition)
    if re.search(r"\|\||[?,()]", flat):
        return False
    return any((m := GUARD_TERM.fullmatch(term.strip())) is not None
               and m.group(1) != "BESS_ARCH_GENERIC"
               for term in flat.split("&&"))


def guarded_lines(lines):
    """[(directive line, guarded after it)], in order: whether code after each
    directive (up to the next) sits inside a branch that _guards()."""
    stack = []
    states = []
    for line_no, directive in logical_directives(lines):
        m = DIRECTIVE.match(directive)
        kind, rest = (m.group(1), m.group(2)) if m else ("", "")
        if kind == "if":
            stack.append(_guards(rest))
        elif kind == "ifdef":
            stack.append(_guards(rest.split()[0] if rest.split() else ""))
        elif kind == "ifndef":
            stack.append(False)
        elif stack and kind == "elif":
            stack[-1] = _guards(rest)
        elif stack and kind == "elifdef":
            stack[-1] = _guards(rest.split()[0] if rest.split() else "")
        elif stack and kind in ("else", "elifndef"):
            stack[-1] = False
        elif stack and kind == "endif":
            stack.pop()
        else:
            continue
        states.append((line_no, any(stack)))
    return states


def kernel_rules(text):
    """Problems (one string each) of an owner kernel file outside core/arch/."""
    clean = strip_comments(text)
    lines = clean.split("\n")
    problems = []
    portable = False
    for line_no, directive in logical_directives(lines):
        if ELSE_LINE.match(directive):
            portable = True
            continue
        if not CONDITION_LINE.match(directive):
            continue
        if GENERIC_MACRO.search(directive):
            portable = True
        raw = sorted({m.group(0) for m in ARCH_MACRO.finditer(directive)
                      if not m.group(0).startswith("BESS_ARCH_")})
        if HAS_INCLUDE_INTRINSIC.search(directive):
            raw.append("__has_include(<intrinsic header>)")
        if raw:
            problems.append(
                f"line {line_no}: condition names {', '.join(raw)}; a kernel file tests "
                f"ISA features only through the BESS_ARCH_* macros of arch/cpu.h "
                f"(add one there if it is missing)")
    found = scan_text(text)
    states = guarded_lines(lines)
    starts = [n for n, _ in states]
    for cat in ("include", "intrinsic", "asm"):
        for line_no in found.get(cat, []):
            # the state after the last directive before this line
            i = bisect.bisect_left(starts, line_no) - 1
            if i < 0 or not states[i][1]:
                problems.append(
                    f"line {line_no}: {cat} outside a BESS_ARCH_* guard; put it in an "
                    f"#if branch that names a cpu.h feature macro (BESS_ARCH_X86, "
                    f"BESS_ARCH_AVX2, ...), so the -Darch_generic build compiles it out")
    if found and not portable:
        problems.append(
            "ISA code without a portable branch: give it an #else (or a condition "
            "naming BESS_ARCH_GENERIC) that the -Darch_generic build compiles and tests")
    return problems


def scan_tree(root):
    """({relative path: {category: [line numbers]}} for every non-kernel file with
    findings, {kernel file: [problems]} for every kernel file, problems or not)."""
    findings = {}
    kernels = {}
    for top in SCAN_DIRS:
        for p in sorted((root / top).rglob("*")):
            if not p.is_file():
                continue
            rel = p.relative_to(root).as_posix()
            if not is_scanned(rel):
                continue
            text = p.read_text(encoding="utf-8", errors="ignore")
            if is_kernel_file(rel):
                kernels[rel] = kernel_rules(text)
                continue
            found = scan_text(text)
            if found:
                findings[rel] = found
    return findings, kernels


def load_allowlist(path):
    data = json.loads(Path(path).read_text(encoding="utf-8"))
    files = data.get("files")
    if not isinstance(files, dict):
        raise SystemExit(f"{path}: no \"files\" object")
    problems = []
    for rel, entry in files.items():
        if not isinstance(entry, dict):
            problems.append(f"{rel}: entry is not an object")
            continue
        if not str(entry.get("reason", "")).strip():
            problems.append(f"{rel}: entry has no reason")
        for key, value in entry.items():
            if key == "reason":
                continue
            if key not in CATEGORIES:
                problems.append(f"{rel}: unknown category {key!r}")
            elif not isinstance(value, int) or isinstance(value, bool) or value <= 0:
                problems.append(f"{rel}: {key} must be a positive integer")
        if rel.startswith(ARCH_DIR):
            problems.append(f"{rel}: core/arch/ is exempt and must not be listed")
        elif is_kernel_file(rel):
            problems.append(f"{rel}: a kernel file is checked by its own rules and must "
                            f"not be listed")
    if problems:
        raise SystemExit(f"{path} is malformed:\n  " + "\n  ".join(problems))
    return files


def compare(findings, allowlist):
    """Problems (one string each) between the scan and the allowlist."""
    problems = []
    for rel in sorted(findings.keys() | allowlist.keys()):
        found = findings.get(rel, {})
        entry = allowlist.get(rel, {})
        for cat in CATEGORIES:
            have = len(found.get(cat, []))
            allowed = entry.get(cat, 0)
            if have > allowed:
                lines = ",".join(str(n) for n in found[cat])
                what = "not allowlisted" if rel not in allowlist else f"allowlist has {allowed}"
                problems.append(
                    f"{rel}: {have} {cat} finding(s), {what} (lines {lines}). Move the "
                    f"architecture-specific code into the owner's <owner>_kernels.h/.cc "
                    f"file, or into core/arch/ if several unrelated owners use it (D-190).")
            elif have < allowed:
                problems.append(
                    f"{rel}: {have} {cat} finding(s), allowlist has {allowed}: the allowlist "
                    f"is stale; lower the entry to {have} (remove it at 0) in this change.")
    return problems


def report(findings, allowlist, comment):
    """The allowlist the current tree would need, for review (`--report`)."""
    files = {}
    for rel in sorted(findings):
        entry = {cat: len(findings[rel][cat]) for cat in CATEGORIES if cat in findings[rel]}
        entry["reason"] = allowlist.get(rel, {}).get("reason", "NEW: give a reason or move it to core/arch/")
        files[rel] = entry
    return json.dumps({"_comment": comment, "files": files}, indent=2) + "\n"


SELF_TEST_FILES = [
    # (path, text, expected {category: count})
    ("core/utils/a.h", "#include <x86intrin.h>\n#include <immintrin.h>\n", {"include": 2}),
    ("core/utils/a.h", "#include <arm_neon.h>\n#include <rte_vect.h>\n#include <cpuid.h>\n",
     {"include": 3}),
    ("core/utils/a.h", '#include "utils/simd.h"\n#include <rte_hash_crc.h>\n', {}),
    ("core/utils/a.h", "#if defined(__x86_64__) && __AVX2__\n#elif __aarch64__\n#endif\n",
     {"condition": 2}),
    ("core/utils/a.h", "#ifdef __SSE4_2__\n#endif\n#ifndef RTE_ARCH_X86\n#endif\n",
     {"condition": 2}),
    ("core/utils/a.h", "#if defined(BESS_ARCH_X86)\n#endif\n", {"condition": 1}),
    ("core/utils/a.h", "#if 1 && \\\n    defined(__ARM_FEATURE_CRC32)\n#endif\n", {"condition": 1}),
    ("core/utils/a.h", "#if __has_include(<immintrin.h>)\n#endif\n", {"condition": 1}),
    # a #define or #error naming the macro is not a condition; INTPTR_MAX is not an arch
    ("core/utils/a.h", "#define X __x86_64__\n#if INTPTR_MAX == INT64_MAX\n#endif\n", {}),
    ("core/utils/a.h", '#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__\n#error "x86"\n#endif\n',
     {}),
    ('core/utils/a.h', 'void f() { asm volatile("pause"); }\n', {"asm": 1}),
    ('core/utils/a.h', 'void f() {\n  asm volatile(\n      "addq %1, %0\\n\\t"\n'
     '      "adcq $0, %0"\n      : "+r"(s) : "g"(x));\n}\n', {"asm": 1}),
    ('core/utils/a.h', 'uint64_t f() { __asm__ __volatile__("rdtsc" : "=a"(lo)); }\n',
     {"asm": 1}),
    ('core/utils/a.h', '#define B() asm volatile("mfence" ::: "memory")\n', {"asm": 1}),
    # portable: the compiler barrier and a symbol label
    ('core/utils/a.h', '#define B() asm volatile("" ::: "memory")\n', {}),
    ('core/utils/a.h', 'extern "C" int f(int)\n    __asm__("real_symbol");\n', {}),
    ("core/utils/a.h", "__m128i v = _mm_loadu_si128(p);\n_mm_storeu_si128(q, v);\n",
     {"intrinsic": 2}),
    ("core/utils/a.h", "uint8x16_t v = vld1q_u8(p);\n", {"intrinsic": 1}),
    ("core/utils/a.h", "x = __rdtsc();\n__builtin_ia32_pause();\nh = __crc32cd(h, v);\n",
     {"intrinsic": 3}),
    ("core/utils/a.h", "h = crc32c_sse42_u64(v, h);\n", {"intrinsic": 1}),
    ("core/utils/a.h", '__attribute__((target("avx2"))) void f();\n'
     'if (__builtin_cpu_supports("avx2")) {}\n', {"intrinsic": 2}),
    # comments are not code; strings are not intrinsics
    ("core/utils/a.h", "// _mm_pause() and #include <x86intrin.h>\n"
     "/* asm volatile(\"pause\");\n#if __AVX2__\n*/\n", {}),
    ("core/utils/a.h", 'const char *s = "_mm_pause // not a comment";\n', {}),
    ("core/utils/a.h", "int mm_count = 0; uint64_t rte_rdtsc_x = rte_rdtsc();\n", {}),
    # everything at once, in a benchmark: tests and benchmarks are covered
    ("core/flow/flow_bench.cc",
     "#include <x86intrin.h>\n#ifdef __x86_64__\nauto t = __rdtscp(&a);\n#endif\n", {
         "include": 1, "condition": 1, "intrinsic": 1}),
]

# (text of an owner kernel file outside core/arch/, expected problem count)
KERNEL_PORTABLE = ("#if defined(BESS_ARCH_X86)\n#include <immintrin.h>\n#endif\n"
                   "unsigned f() {\n#if defined(BESS_ARCH_AVX2)\n"
                   "  return _mm256_movemask_epi8(v);\n#else\n  return 0;\n#endif\n}\n")
SELF_TEST_KERNELS = [
    # intrinsics behind BESS_ARCH_* with an #else: allowed
    (KERNEL_PORTABLE, 0),
    # a condition naming BESS_ARCH_GENERIC is a portable branch too
    ("#if !defined(BESS_ARCH_GENERIC) && defined(BESS_ARCH_X86)\n"
     "inline void f() { _mm_pause(); }\n#endif\n", 0),
    # vector extensions and no ISA condition: nothing to check
    ("using V = uint8_t __attribute__((vector_size(16)));\n", 0),
    # raw compiler and DPDK macros are refused, one problem per condition (and
    # the intrinsic under __AVX2__ is not guarded by a BESS_ARCH_* macro)
    (KERNEL_PORTABLE.replace("BESS_ARCH_AVX2", "__AVX2__"), 2),
    ("#if defined(__x86_64__) && defined(__SSE4_2__)\n#elif defined(__aarch64__)\n"
     "#else\n#endif\n", 2),
    ("#ifdef RTE_ARCH_X86\n#else\n#endif\n", 1),
    ("#if __has_include(<immintrin.h>)\n#else\n#endif\n", 1),
    # in a comment it is not a condition
    ("// #if __AVX2__\n#if defined(BESS_ARCH_X86)\n#else\n#endif\n", 0),
    # ISA code with no portable branch
    ("#if defined(BESS_ARCH_X86)\ninline void f() { _mm_pause(); }\n#endif\n", 1),
    ('inline void f() { asm volatile("pause"); }\n', 2),
    # a raw macro, an unguarded intrinsic and no portable branch
    ("#ifdef __AVX2__\n__m256i v;\n#endif\n", 3),
    # unguarded include and intrinsic; an unrelated #else does not guard them
    ("#include <immintrin.h>\ninline void f() { _mm_pause(); }\n"
     "#ifndef NDEBUG\n#else\n#endif\n", 2),
    # guarded only by an unrelated condition
    ("#ifndef NDEBUG\ninline void f() { _mm_pause(); }\n#else\n#endif\n", 1),
    ("#if defined(NDEBUG)\ninline void f() { _mm_pause(); }\n#else\n#endif\n", 1),
    # a nested guard counts, at any depth; #ifdef of a feature macro too
    ("#if defined(BESS_ARCH_X86)\n#ifndef NDEBUG\ninline void f() { _mm_pause(); }\n"
     "#endif\n#else\n#endif\n", 0),
    ("#ifdef BESS_ARCH_SSE4_2\n#include <nmmintrin.h>\n#else\n#endif\n", 0),
    # negated, in the #else branch, or after the #endif: not guarded
    ("#if !defined(BESS_ARCH_X86)\ninline void f() { _mm_pause(); }\n#else\n#endif\n", 1),
    ("#ifndef BESS_ARCH_X86\ninline void f() { _mm_pause(); }\n#else\n#endif\n", 1),
    ("#if defined(BESS_ARCH_X86)\n#else\ninline void f() { _mm_pause(); }\n#endif\n", 1),
    ("#if defined(BESS_ARCH_X86)\n#else\n#endif\ninline void f() { _mm_pause(); }\n", 1),
    # an #elif branch is judged by its own condition
    ("#if defined(NDEBUG)\n#elif defined(BESS_ARCH_ARM64)\n"
     'inline void f() { asm volatile("yield"); }\n#else\n#endif\n', 0),
    ("#if defined(NDEBUG)\n#elif 1 || defined(BESS_ARCH_ARM64)\n"
     'inline void f() { asm volatile("yield"); }\n#else\n#endif\n', 1),
    # a condition that can be true without the feature macro guards nothing:
    # `||` anywhere, or `!` before a group
    ("#if !(defined(BESS_ARCH_X86))\ninline void f() { _mm_pause(); }\n#else\n#endif\n", 1),
    ("#if defined(BESS_ARCH_X86) || defined(BESS_ARCH_GENERIC)\n"
     "inline void f() { _mm_pause(); }\n#else\n#endif\n", 1),
    ("#if 1 || defined(BESS_ARCH_X86)\ninline void f() { _mm_pause(); }\n#else\n#endif\n", 1),
    # `&&` with another term still guards
    ("#if defined(BESS_ARCH_X86) && !defined(NDEBUG)\ninline void f() { _mm_pause(); }\n"
     "#else\n#endif\n", 0),
    # BESS_ARCH_GENERIC alone guards nothing: it says when to compile the
    # portable code, not that a feature is there
    ("#if !defined(BESS_ARCH_GENERIC)\ninline void f() { _mm_pause(); }\n#endif\n", 1),
    # true without the feature macro: comparison, bitwise, ternary, a group
    ("#if defined(BESS_ARCH_X86) == 0\ninline void f() { _mm_pause(); }\n#else\n#endif\n", 1),
    ("#if defined(BESS_ARCH_X86) | 1\ninline void f() { _mm_pause(); }\n#else\n#endif\n", 1),
    ("#if defined(BESS_ARCH_X86) ? 1 : 1\ninline void f() { _mm_pause(); }\n#else\n#endif\n", 1),
    ("#if (defined(BESS_ARCH_X86) && 1) == 0\ninline void f() { _mm_pause(); }\n"
     "#else\n#endif\n", 1),
    # accepted forms: bare name, `defined M`, and `&&` chains
    ("#if BESS_ARCH_X86 && !defined(NDEBUG)\ninline void f() { _mm_pause(); }\n"
     "#else\n#endif\n", 0),
    ("#if defined BESS_ARCH_AVX2\n__m256i v;\n#else\n#endif\n", 0),
    ("#if defined ( BESS_ARCH_AVX2 )\n__m256i v;\n#else\n#endif\n", 0),
    # comments on a condition line are blanked first: they neither reject a
    # guard nor make one
    ("#if defined(BESS_ARCH_X86) /* (x) || y */ // ? z\ninline void f() { _mm_pause(); }\n"
     "#else\n#endif\n", 0),
    ("#if 1 /* defined(BESS_ARCH_X86) */\ninline void f() { _mm_pause(); }\n#else\n#endif\n", 1),
    ("#ifdef BESS_ARCH_X86 // the x86 path\ninline void f() { _mm_pause(); }\n#else\n#endif\n", 0),
]


def run_self_test():
    print("Running check_arch self-test...")
    for path, text, want in SELF_TEST_FILES:
        got = {c: len(v) for c, v in scan_text(text).items()}
        if got != want:
            raise AssertionError(f"{path}: {text!r}: want {want}, got {got}")

    # core/arch/ is exempt; core/ elsewhere, the sample plugin and examples are not
    for rel, want in [("core/arch/cpu.h", False), ("core/arch_test.cc", True),
                      ("core/utils/copy.h", True), ("core/flow/flow_bench.cc", True),
                      ("sample_plugin/modules/x.cc", True), ("examples/p/x.cc", True),
                      ("tools/x.cc", False), ("core/README.md", False)]:
        if is_scanned(rel) != want:
            raise AssertionError(f"is_scanned({rel}) should be {want}")

    # owner kernel files: <owner>_kernels.h/.cc in a scanned tree, not their
    # tests, not core/arch/ (exempt as a whole), not other names
    kernel_paths = [("core/l2/mac_table_kernels.h", True),
                    ("core/modules/vlan_kernels.cc", True),
                    ("sample_plugin/modules/x_kernels.h", True),
                    ("examples/p/x_kernels.cc", True),
                    ("core/l2/mac_table_kernels_test.cc", False),
                    ("core/arch/checksum_kernels.h", False),
                    ("core/l2/kernels.h", False), ("core/l2/x_kernels.inc", False),
                    ("tools/x_kernels.h", False)]
    for rel, want in kernel_paths:
        if is_kernel_file(rel) != want:
            raise AssertionError(f"is_kernel_file({rel}) should be {want}")
    for text, want in SELF_TEST_KERNELS:
        got = kernel_rules(text)
        if len(got) != want:
            raise AssertionError(f"kernel_rules: {text!r}: want {want}, got {got}")

    # end to end: the same intrinsic passes in a kernel file and is a finding
    # in the module's logic file
    with tempfile.TemporaryDirectory() as d:
        root = Path(d)
        (root / "core" / "modules").mkdir(parents=True)
        (root / "core" / "modules" / "m_kernels.h").write_text(KERNEL_PORTABLE)
        (root / "core" / "modules" / "m.cc").write_text(KERNEL_PORTABLE)
        findings, kernels = scan_tree(root)
        if (kernels != {"core/modules/m_kernels.h": []}
                or set(findings) != {"core/modules/m.cc"}):
            raise AssertionError(f"scan_tree: got {findings}, {kernels}")

    findings = {"core/a.h": {"asm": [3], "include": [1]}}
    exact = {"core/a.h": {"asm": 1, "include": 1, "reason": "r"}}
    for allow, want_problems, label in [
        (exact, 0, "exact counts pass"),
        ({"core/a.h": {"asm": 1, "reason": "r"}}, 1, "growth in a listed file fails"),
        ({}, 2, "an unlisted file fails"),
        ({"core/a.h": {"asm": 2, "include": 1, "reason": "r"}}, 1, "a stale count fails"),
        ({**exact, "core/b.h": {"asm": 1, "reason": "r"}}, 1, "a stale file fails"),
    ]:
        got = compare(findings, allow)
        if len(got) != want_problems:
            raise AssertionError(f"compare: {label}: want {want_problems}, got {got}")

    # a malformed allowlist is refused: every entry needs a reason, known
    # categories with positive counts, and neither core/arch/ nor a kernel
    # file is ever listed
    malformed = [
        {"core/a.h": {"asm": 1}},
        {"core/a.h": {"asm": 1, "reason": " "}},
        {"core/a.h": {"cpuid": 1, "reason": "r"}},
        {"core/a.h": {"asm": 0, "reason": "r"}},
        {"core/a.h": {"asm": True, "reason": "r"}},
        {"core/arch/cpu.h": {"asm": 1, "reason": "r"}},
        {"core/l2/mac_table_kernels.h": {"asm": 1, "reason": "r"}},
    ]
    with tempfile.TemporaryDirectory() as d:
        path = Path(d) / "allow.json"
        path.write_text(json.dumps({"files": exact}))
        if load_allowlist(path) != exact:
            raise AssertionError("a well-formed allowlist must load")
        for files in malformed:
            path.write_text(json.dumps({"files": files}))
            try:
                load_allowlist(path)
            except SystemExit:
                continue
            raise AssertionError(f"malformed allowlist accepted: {files}")

    print(f"Self-test PASSED: {len(SELF_TEST_FILES)} scan cases, "
          f"{len(SELF_TEST_KERNELS)} kernel-file cases, "
          f"{8 + len(kernel_paths)} path cases, 1 tree case, "
          f"5 allowlist cases, {len(malformed)} malformed allowlists refused.")


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", default=str(ROOT), help="repository root")
    parser.add_argument("--allowlist", default=None,
                        help="allowlist JSON (default: <root>/tools/arch_allowlist.json)")
    parser.add_argument("--report", action="store_true",
                        help="print the current counts in the allowlist format (stdout)")
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--verbose", "-v", action="store_true",
                        help="list every finding with its line")
    args = parser.parse_args()

    if args.self_test:
        run_self_test()
        return 0

    root = Path(args.root)
    allowlist_path = Path(args.allowlist) if args.allowlist else root / "tools" / "arch_allowlist.json"
    findings, kernels = scan_tree(root)
    allowlist = load_allowlist(allowlist_path)

    if args.report:
        comment = json.loads(allowlist_path.read_text(encoding="utf-8")).get("_comment", [])
        sys.stdout.write(report(findings, allowlist, comment))
        return 0

    if args.verbose:
        for rel in sorted(findings):
            for cat, lines in sorted(findings[rel].items()):
                print(f"  {rel}: {cat}: lines {','.join(map(str, lines))}")

    problems = compare(findings, allowlist)
    problems += [f"{rel}: {p}" for rel in sorted(kernels) for p in kernels[rel]]
    if problems:
        print(f"FAILED: architecture code outside core/arch/ breaks D-071/D-190 or differs "
              f"from {allowlist_path.name} ({len(problems)} problem(s)):", file=sys.stderr)
        for p in problems:
            print(f"  {p}", file=sys.stderr)
        return 1
    total = sum(len(v) for f in findings.values() for v in f.values())
    print(f"OK: {len(kernels)} owner kernel file(s) follow the kernel rules; "
          f"{total} grandfathered architecture finding(s) in {len(findings)} other "
          "file(s) outside core/arch/ match the allowlist; no growth.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
