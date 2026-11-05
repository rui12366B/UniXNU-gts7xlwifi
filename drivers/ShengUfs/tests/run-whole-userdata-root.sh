#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause-Patent
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cxx=${CXX:-/usr/bin/clang++}

if ! "$cxx" --version | grep -q 'Apple clang'; then
  echo "error: WholeUserdataRoot tests require Apple clang (got $cxx)" >&2
  exit 1
fi

tmp=${TMPDIR:-/tmp}
build=$(mktemp -d "$tmp/whole-userdata-root.XXXXXX")
trap 'rm -rf "$build"' EXIT HUP INT TERM

"$cxx" -std=c++17 -Wall -Wextra -Werror -pedantic \
  "$test_dir/WholeUserdataRoot.cpp" -o "$build/WholeUserdataRoot"
"$build/WholeUserdataRoot"

"$cxx" -std=c++17 -Wall -Wextra -Werror -pedantic \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  "$test_dir/WholeUserdataRoot.cpp" -o "$build/WholeUserdataRoot-sanitized"
"$build/WholeUserdataRoot-sanitized"
