#!/usr/bin/env python3
#
#    Copyright (c) 2026 Project CHIP Authors
#    All rights reserved.
#
#    Licensed under the Apache License, Version 2.0 (the "License");
#    you may not use this file except in compliance with the License.
#    You may obtain a copy of the License at
#
#        http://www.apache.org/licenses/LICENSE-2.0
#
#    Unless required by applicable law or agreed to in writing, software
#    distributed under the License is distributed on an "AS IS" BASIS,
#    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
#    See the License for the specific language governing permissions and
#    limitations under the License.

"""Adapt an upstream Matter crypto/credentials test source (test body or vector
header) to native-MSVC C++17 at build time, leaving the upstream file untouched.

The upstream src/crypto/tests and src/credentials/tests sources are written for
GCC/Clang, which accept several extensions in C++17 mode that MSVC accepts only
under /std:c++20 or not at all. This script performs three mechanical,
semantics-preserving rewrites so the sources compile under the repository's
/std:c++17 contract with MSVC:

  1. C99/GCC compound literals ((const T[]){ ... }) used to initialize a Span --
     unsupported by MSVC in C++ -- become an equivalent named constexpr array.

  2. Unsized empty arrays (T name[] = {}), which GCC/Clang allow but MSVC rejects
     (C2466), get a single unused placeholder element (T name[] = { 0 }). Any
     sizeof(name) that referred to the now-nonempty array is rewritten to 0 so
     the paired length stays 0 and the placeholder byte is never read.

  3. C++20 designated-initializer aggregates ({ .field = value, ... }) -- which
     GCC/Clang accept as a C++17 extension but MSVC rejects (C7555) -- become
     the equivalent C++17 positional aggregate initialization ({ value, ... }).
     Only designators that immediately follow '{' or ',' are stripped, so member
     assignments (obj.field = x) elsewhere in test-body code are left untouched.

The output is a build artifact; the upstream file remains the source of truth.
Correctness of every rewrite is validated at runtime by the known-answer tests
themselves: any misaligned vector would fail its assertion.
"""

import argparse
import re
import sys


def convert_compound_literals(text: str) -> str:
    # extern constexpr ByteSpan NAME((const uint8_t[]){ bytes });
    text = re.sub(
        r"extern constexpr ByteSpan (\w+)\(\(const uint8_t\[\]\)\{(.*?)\}\s*\);",
        lambda m: (
            "constexpr uint8_t %s__cxx17_arr[] = {%s};\n"
            "extern constexpr ByteSpan %s(%s__cxx17_arr);"
            % (m.group(1), m.group(2), m.group(1), m.group(1))
        ),
        text,
        flags=re.DOTALL,
    )
    # const Span<const ByteSpan> NAME((const ByteSpan[]){ elements });
    text = re.sub(
        r"const Span<const ByteSpan>\s+(\w+)\(\(const ByteSpan\[\]\)\{(.*?)\}\s*\);",
        lambda m: (
            "constexpr ByteSpan %s__cxx17_arr[] = {%s};\n"
            "const Span<const ByteSpan> %s(%s__cxx17_arr);"
            % (m.group(1), m.group(2), m.group(1), m.group(1))
        ),
        text,
        flags=re.DOTALL,
    )
    return text


def fix_unsized_empty_arrays(text: str) -> str:
    empty_names = set()

    def record(m):
        empty_names.add(m.group("name"))
        return "%s%s{ 0 }" % (m.group("name"), m.group("mid"))

    # Only unsized arrays: NAME[] = {}. Sized arrays (NAME[8] = {}) are valid.
    text = re.sub(
        r"(?P<name>\b\w+)(?P<mid>\s*\[\s*\]\s*=\s*)\{\s*\}",
        record,
        text,
    )
    for name in empty_names:
        text = re.sub(r"sizeof\s*\(\s*%s\s*\)" % re.escape(name), "0", text)
    return text


def strip_designators(text: str) -> str:
    # A designator only follows '{' or ',' in an initializer list (possibly with
    # intervening whitespace and // or /* */ comments). Member assignments
    # (obj.field = x) are preceded by an identifier / ')' / ']', so they are not
    # matched and are left untouched.
    gap = r"(?:\s|//[^\n]*\n|/\*.*?\*/)*"
    return re.sub(
        r"([{,])(" + gap + r")\.\w+\s*=\s*",
        r"\1\2",
        text,
        flags=re.DOTALL,
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("input")
    parser.add_argument("output")
    args = parser.parse_args()

    with open(args.input, "r", encoding="utf-8") as f:
        text = f.read()

    text = convert_compound_literals(text)
    text = fix_unsized_empty_arrays(text)
    text = strip_designators(text)

    with open(args.output, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
