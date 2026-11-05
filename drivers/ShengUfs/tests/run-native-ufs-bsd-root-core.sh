#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause-Patent
set -eu

test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$test_dir/.." && pwd)
build=${BUILD_DIR:-"$root/out/native-ufs-bsd-root-core"}
cxx=${CXX:-c++}

mkdir -p "$build"

common="-std=c++17 -Wall -Wextra -Werror -Wpedantic -I$root"
"$cxx" $common "$root/NativeUfsBsdRootCore.cpp" \
  "$test_dir/NativeUfsBsdRootCore.cpp" -o "$build/NativeUfsBsdRootCore"
"$build/NativeUfsBsdRootCore"

"$cxx" $common -fsanitize=address,undefined -fno-omit-frame-pointer \
  "$root/NativeUfsBsdRootCore.cpp" "$test_dir/NativeUfsBsdRootCore.cpp" \
  -o "$build/NativeUfsBsdRootCore-sanitized"
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
  "$build/NativeUfsBsdRootCore-sanitized"

echo "PASS: NativeUfs BSD-root core host and ASan/UBSan"
