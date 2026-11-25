#!/usr/bin/env bash
set -euo pipefail
cd /src
git rev-parse HEAD > /out/source-sha.txt
printf '%s\n' all > /out/test-suite.txt
common=(release --clang21 --with-pam --with-odyssey -j4)
./yb_build.sh "${common[@]}" --collect-java-tests
# initdb must complete before invoking tests
./yb_build.sh "${common[@]}" initdb --skip-java
grep -Eq '^#define[[:space:]]+USE_PAM[[:space:]]+1$' \
  build/latest/postgres/include/server/pg_config.h

status=0
./yb_build.sh "${common[@]}" --skip-build --ctest-args \
  '--parallel 2 --output-on-failure --no-tests=error --output-junit /out/ctest.xml' || status=1

# retain each invocation's exit status instead of the --fail-never aggregate path
[[ -s build/latest/java_test_list.txt ]]
cp build/latest/java_test_list.txt /out/java-test-list.txt
sort -u /out/java-test-list.txt > /out/java-test-list-sorted.txt
while IFS= read -r test_method; do
  [[ -n $test_method ]] || continue
  ./yb_build.sh "${common[@]}" --skip-build --java-test "$test_method" || status=1
done < /out/java-test-list-sorted.txt
python3 /ci/check_reports.py /src/java || status=1
exit "$status"
