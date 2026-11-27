#!/usr/bin/env bash
set -euo pipefail
cd /src
suite=${1:-all}
common=(release --clang21 --with-pam --with-odyssey -j4)
source_sha=$(git rev-parse HEAD)
# invalidate each stamp before changing its build outputs
rm -f build/cpp-test-source-sha.txt
if [[ $suite == cpp ]]; then
  ./yb_build.sh "${common[@]}" --skip-java
else
  rm -f build/java-test-source-sha.txt
  # resolve Maven build extensions before spending time on C++ compilation
  bash /ci/check_maven.sh /out
  ./yb_build.sh "${common[@]}" --collect-java-tests
fi
# initdb must complete before invoking tests
./yb_build.sh "${common[@]}" initdb --skip-java
grep -Eq '^#define[[:space:]]+USE_PAM[[:space:]]+1$' \
  build/latest/postgres/include/server/pg_config.h

source ./build-support/common-test-env.sh
detect_architecture
prep_ybc_testing
for tool in yb-controller-server yb-controller-cli; do
  if [[ ! -x build/ybc/$tool ]]; then
    echo "Missing YB Controller executable: build/ybc/$tool" >&2
    exit 1
  fi
done

if [[ $suite != cpp ]]; then
  cp "$HOME/.m2/settings.xml" build/maven-settings.xml
  printf '%s\n' "$source_sha" > build/java-test-source-sha.txt
fi
printf '%s\n' "$source_sha" > build/cpp-test-source-sha.txt
