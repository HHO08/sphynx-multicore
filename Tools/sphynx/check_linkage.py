#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
check_linkage.py — no C++ types may leak out of an extern "C" block.

clang (the compiler the Android NDK build uses) turns this into an error:

    error: 'SaveStateToSlot' has C-linkage specified, but returns user-defined
    type 'std::string' which is incompatible with C
    [-Wreturn-type-c-linkage]

GCC does not warn about it, so the local type-check passed while the APK build
failed 13 minutes later. This script closes that gap: it walks every extern "C"
block in extcores/ and fails if a function in one of them returns a C++ type.

Exit code 0 = clean. SPDX-License-Identifier: GPL-2.0-or-later
"""

import re
import sys
from pathlib import Path

CPP_TYPES = re.compile(
    r"^\s*(?:static\s+)?(?:const\s+)?(std::(?:string|vector|map|unique_ptr|shared_ptr|function|array|pair)\b"
    r"|[A-Za-z_][A-Za-z0-9_]*::[A-Za-z_][A-Za-z0-9_]*\s*&)"
)
FUNC_LINE = re.compile(r"^\s*(?:static\s+)?[A-Za-z_][A-Za-z0-9_:<>,\s\*&]*\s+\**[A-Za-z_][A-Za-z0-9_]*\s*\(")


def extern_c_blocks(text: str):
    """Yields (start_offset, end_offset) for every extern "C" { ... } block."""
    for m in re.finditer(r'extern\s+"C"\s*\{', text):
        depth = 0
        i = m.end() - 1
        while i < len(text):
            if text[i] == "{":
                depth += 1
            elif text[i] == "}":
                depth -= 1
                if depth == 0:
                    yield (m.end(), i)
                    break
            i += 1


def looks_like_function(line: str) -> bool:
    """Distinguish `std::string f(int a)` from the variable `std::string f(g);`.

    A parameter list either is empty, says void, or names a type and a variable
    (which needs a space, a comma or a pointer/reference mark). A single bare
    identifier in the parentheses is an initialiser, i.e. a variable.
    """
    if "(" not in line:
        return False
    start = line.index("(")
    depth = 0
    end = -1
    for i in range(start, len(line)):
        if line[i] == "(":
            depth += 1
        elif line[i] == ")":
            depth -= 1
            if depth == 0:
                end = i
                break
    if end < 0:
        return False
    args = line[start + 1:end].strip()
    if args == "" or args == "void":
        return True
    return (" " in args) or ("," in args) or ("*" in args) or ("&" in args)


def main(root: str) -> int:
    repo = Path(root).resolve()
    problems = []
    scanned = 0
    for path in sorted(repo.glob("extcores/**/*.cpp")) + sorted(repo.glob("extcores/**/*.h")):
        text = path.read_text(encoding="utf-8", errors="surrogateescape")
        if 'extern "C"' not in text:
            continue
        scanned += 1
        for start, end in extern_c_blocks(text):
            for offset, line in enumerate(text[start:end].splitlines(), start=1):
                # Every function inside an extern "C" block is written at column
                # 0. An indented line is part of a body, where C++ types are
                # completely legal - stage 9's player_core.cpp builds a
                # std::unique_ptr on the first line of an exported function.
                if line[:1] in (" ", "\t"):
                    continue
                if not CPP_TYPES.match(line):
                    continue
                if not FUNC_LINE.match(line):
                    continue
                if not looks_like_function(line):
                    continue
                if "=" in line.split("(")[0]:
                    continue
                rel = path.relative_to(repo)
                problems.append(f"{rel}: C++ return type inside extern \"C\": {line.strip()}")

    if problems:
        print("C-LINKAGE CHECK FAILED:")
        for p in problems:
            print("  - " + p)
        print("\nMove the helper into a C++ block (see player_android.cpp) or return a")
        print("plain C type such as 'const char *'.")
        return 1

    print(f"c-linkage check: {scanned} files with extern \"C\" blocks, no C++ types escaped")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "."))
