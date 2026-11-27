#!/usr/bin/env bash
# run from the Actions workspace after prepare-tests completes
set -euo pipefail
shopt -s nullglob
targets=(source/java/*/target)
(( ${#targets[@]} > 0 ))
git -C source rev-parse HEAD | cmp - source/build/cpp-test-source-sha.txt
cmp source/build/cpp-test-source-sha.txt source/build/java-test-source-sha.txt
[[ -d maven-repository/org/yb ]]
# keep the container's absolute symlinks and executable modes
# the test wrapper fetches its pinned bash helper, third-party libraries and LLVM
tar --exclude='source/build/yugabyte-bash-common' --exclude='*.o' \
    --exclude='*/target/surefire-reports*' --exclude='*/yb-test-logs' \
    -cf - source/build "${targets[@]}" maven-repository |
  zstd -q -T2 -1 -o test-bundle.tar.zst
