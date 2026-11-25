#!/usr/bin/env bash
set -euo pipefail
cd /src

if [[ ! ${RELEASE_TAG:-} =~ ^v([0-9]+\.[0-9]+\.[0-9]+\.[0-9]+)-(deepbi[0-9]+)$ ]]; then
  echo 'Expected a release tag such as v2026.1.2.0-deepbi1' >&2
  exit 2
fi
base_version=${BASH_REMATCH[1]}
build_number=${BASH_REMATCH[2]}
version=${RELEASE_TAG#v}
source_sha=$(git rev-parse HEAD)
[[ $source_sha == "${SOURCE_SHA:?source SHA required}" ]]
[[ $(git rev-parse "refs/tags/$RELEASE_TAG^{commit}") == "$source_sha" ]]
[[ $(tr -d '\r\n' < version.txt) == "$version" ]]
[[ -z $(git status --porcelain --untracked-files=no) ]]
mkdir -p /out/assets

{
  cat /etc/os-release
  python3 --version
  cmake --version
  rustc --version
  cargo --version
  java -version
} > /out/tool-versions.txt 2>&1

# resolve Maven build extensions before spending time on C++ compilation
mvn --batch-mode --non-recursive --file java/pom.xml validate
YB_RELEASE_BUILD_NUMBER="$build_number" ./yb_release \
  --force --build release \
  --save_release_path_to_file /out/package-path.txt \
  --build_args='--clang21 --full-lto --with-pam -j4'

# use the path returned by the packager rather than reconstructing its filename
package=$(cat /out/package-path.txt)
[[ -f $package ]]
asset="yugabyte-${version}-el9-x86_64.tar.gz"
mv -- "$package" "/out/assets/$asset"
python3 - "$source_sha" "$base_version" "$build_number" "$asset" <<'PY'
import json
import pathlib
import sys

sha, version, build_number, asset = sys.argv[1:]
metadata = json.loads(pathlib.Path('build/latest/version_metadata.json').read_text())
assert metadata['git_hash'] == sha, metadata
assert metadata['version_number'] == version, metadata
assert metadata['build_number'] == build_number, metadata
manifest = {
    'source_sha': sha,
    'release_tag': f'v{version}-{build_number}',
    'asset': asset,
    'build_os': 'almalinux:9',
    'build_args': '--clang21 --full-lto --with-pam -j4',
    'thirdparty_url': pathlib.Path('build/latest/thirdparty_url.txt').read_text().strip(),
    'version_metadata': metadata,
}
pathlib.Path('/out/assets/build-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
PY
cd /out/assets
sha256sum "$asset" build-manifest.json > SHA256SUMS
