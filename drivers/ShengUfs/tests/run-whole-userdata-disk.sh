#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause-Patent
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cxx=${CXX:-/usr/bin/clang++}

if ! "$cxx" --version | grep -q 'Apple clang'; then
  echo "error: WholeUserdataDisk tests require Apple clang (got $cxx)" >&2
  exit 1
fi

tmp=${TMPDIR:-/tmp}
build=$(mktemp -d "$tmp/whole-userdata-disk.XXXXXX")
trap 'rm -rf "$build"' EXIT HUP INT TERM

"$cxx" -std=c++17 -Wall -Wextra -Werror -pedantic \
  "$test_dir/WholeUserdataDisk.cpp" -o "$build/WholeUserdataDisk"
"$build/WholeUserdataDisk"

"$cxx" -std=c++17 -Wall -Wextra -Werror -pedantic \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  "$test_dir/WholeUserdataDisk.cpp" -o "$build/WholeUserdataDisk-sanitized"
"$build/WholeUserdataDisk-sanitized"
