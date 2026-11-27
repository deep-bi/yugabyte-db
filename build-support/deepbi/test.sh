#!/usr/bin/env bash
set -euo pipefail
suite=${TEST_SUITE:-all}
shard=${TEST_SHARD_INDEX:-1}
shard_count=${TEST_SHARD_COUNT:-1}
case "$suite" in
  all|cpp|java|probe) ;;
  *) echo "Unknown test suite: $suite" >&2; exit 2 ;;
esac
if [[ ! $shard =~ ^[1-9][0-9]*$ || ! $shard_count =~ ^[1-9][0-9]*$ ]] ||
   (( shard > shard_count )); then
  echo "Invalid test shard: $shard/$shard_count" >&2
  exit 2
fi
cd /src
# match the container's hard core limit
export YB_TEST_ULIMIT_CORE=0
git rev-parse HEAD > /out/source-sha.txt
printf '%s\n' "$suite" > /out/test-suite.txt
printf '%s/%s\n' "$shard" "$shard_count" > /out/test-shard.txt
common=(release --clang21 --with-pam --with-odyssey -j4)
if [[ ${1:-} == --prebuilt ]]; then
  # Java tests also start servers from the native build
  cmp /out/source-sha.txt build/cpp-test-source-sha.txt
else
  bash /ci/prepare-tests.sh "$suite"
fi

status=0
if [[ $suite == probe ]]; then
  cpp_test=ann_methods_index_merge-test
  ./yb_build.sh "${common[@]}" --skip-build --ctest-args \
    "-R ^$cpp_test$ --no-tests=error --output-on-failure --output-junit /out/ctest.xml" || status=1
elif [[ $suite != java ]]; then
  ctest --test-dir build/latest --show-only=json-v1 > /out/ctest-tests.json
  test_count=$(python3 -c 'import json, sys; print(len(json.load(open(sys.argv[1]))["tests"]))' \
    /out/ctest-tests.json)
  # ctest treats an empty index selection as all tests
  if (( shard > test_count )); then
    echo "No C++ tests in shard $shard/$shard_count ($test_count registered)" >&2
    exit 1
  fi
  # interleave registered test binaries across runners
  ctest --test-dir build/latest --show-only -I "$shard,,$shard_count" \
    > /out/ctest-list.txt
  ./yb_build.sh "${common[@]}" --skip-build --ctest-args \
    "--parallel 1 -I $shard,,$shard_count --output-on-failure --no-tests=error --output-junit /out/ctest.xml" \
    || status=1
fi

if [[ $suite == cpp ]]; then
  exit "$status"
fi
if [[ ${1:-} == --prebuilt ]]; then
  cmp /out/source-sha.txt build/java-test-source-sha.txt
  cp build/maven-settings.xml "$HOME/.m2/settings.xml"
fi
if [[ $suite == probe ]]; then
  java_test='org.yb.pgsql.TestPgSelect#testWhereClause'
  if ! grep -Fxq "$java_test" build/latest/java_test_list.txt; then
    echo "Java probe missing from collected tests: $java_test" >&2
    exit 1
  fi
  printf '%s\n' "$java_test" > /out/java-test-list-shard.txt
else
  [[ -s build/latest/java_test_list.txt ]]
  cp build/latest/java_test_list.txt /out/java-test-list.txt
  sed '/^[[:space:]]*$/d' /out/java-test-list.txt | LC_ALL=C sort -u \
    > /out/java-test-list-sorted.txt
  awk -v shard="$shard" -v count="$shard_count" '(NR - 1) % count == shard - 1' \
    /out/java-test-list-sorted.txt > /out/java-test-list-shard.txt
  [[ -s /out/java-test-list-shard.txt ]]
fi

YB_TEST_ARTIFACT_LIST_PATH=$(mktemp)
export YB_TEST_ARTIFACT_LIST_PATH
trap 'rm -f "$YB_TEST_ARTIFACT_LIST_PATH"' EXIT
while IFS= read -r test_method; do
  # --java-test enables the Java build, so --skip-build must follow it
  ./yb_build.sh "${common[@]}" --java-test "$test_method" --skip-build || status=1
done < /out/java-test-list-shard.txt
python3 /ci/check_reports.py "$YB_TEST_ARTIFACT_LIST_PATH" || status=1
exit "$status"
