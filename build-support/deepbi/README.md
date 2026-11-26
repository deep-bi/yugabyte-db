# DeepBI GitHub Actions

Two manual workflows run on standard public-repository `ubuntu-24.04` runners.
Compilation runs as an unprivileged user in AlmaLinux 9 with Clang 21, PAM,
and four build jobs. The release build uses full LTO. Source tests use dynamic
linking and enable Odyssey.

The current dependency manifest has an AlmaLinux 9 Clang 21 archive for dynamic
builds. Its full-LTO archive is built on AlmaLinux 8, which the upstream selector
allows on newer compatible systems. The release manifest records the selected
third-party archive URL. An EL9 package identifies the database build target,
not the build platform of every prebuilt dependency.

CMake is pinned to 3.31.6. The original recipe's `cmake==3.31.0` could not be
resolved by pip during the container check.

The first hosted run must establish whether the build fits. These workflows do
not establish a measured memory requirement, disk requirement, or completion
time. The upstream LTO linker waits for 13 GiB of available RAM. The build/test
step stops after 330 minutes, leaving time for diagnostics within the hosted
job limit. The complete source suites may require larger runners or sharding.

## First build

Add the workflows to the repository's default branch so GitHub enables manual
dispatch. The branch selected in the Actions UI supplies the workflow scripts.
The release tag supplies the database source, so existing tags do not need to
contain these scripts.

Run **DeepBI build and release** with:

- `release_tag`: `v2026.1.2.0-deepbi1`, or another existing `vX.Y.Z.W-deepbiN` tag
- `publish_release`: `false`

The tag's `version.txt` must contain the matching `X.Y.Z.W-deepbiN` version.
The workflow sets `YB_RELEASE_BUILD_NUMBER=deepbiN` and checks the generated
version metadata against the selected commit and version.

The build uploads `deepbi-package-<run id>-<attempt>` containing:

- `yugabyte-X.Y.Z.W-deepbiN-el9-x86_64.tar.gz`
- `build-manifest.json` with the source commit, version metadata, build flags,
  and selected third-party archive URL
- `SHA256SUMS` covering the package and manifest

A fresh job downloads this exact artifact and tests it in Rocky Linux 9.
It verifies checksums and embedded metadata, runs `post_install.sh`, starts a
single master and tserver, writes and reads SQL data, and tests real local PAM
authentication with correct and incorrect passwords. The authentication checks
cover direct YSQL and both Connection Manager authentication modes. Data is
read again after each tserver restart. Test-only PAM libraries must be absent
from the package.

The smoke image has a disposable `ybci` account with a fixed test password and
a private `/etc/pam.d/yb-ci` service using `pam_unix`. No ports are published to
the host. This checks local PAM integration on Rocky 9, not SSSD, AD, production
configuration, load, or upgrade behavior.

## Source tests

Run **DeepBI tests** from the desired branch. Both the workflow tools and database
source use `github.sha`, the branch HEAD when the run is dispatched. That commit
is saved in the diagnostic artifact.

Every run executes both database test suites:

- All registered CTest tests, with two concurrent test processes
- All collected Java test methods, executed individually

PAM regressions run as part of the Java suite. There is no suite selector or
PAM-only test path.

Each invocation's exit status is retained. Java XML reports are checked for
failures, errors, malformed reports, and zero executed tests.

Compilation and tests run in the same container. This workflow builds its own
test binaries and Java classes. The release tarball is not a source-test bundle.

## Publication

Set `publish_release=true` when running the release workflow to create a GitHub
Release after the Rocky 9 smoke job passes. This performs a new build. The
publication job verifies that the tag still points to the built commit and
checks the downloaded checksums again. It does not move tags or replace an
existing release.

Source tests remain independent. Publication requires the package smoke job,
but does not automatically require a separate source-test workflow run. Run
the source-test workflow on the release branch and confirm that the recorded
commit matches the release tag before publication.

Only the publication job has `contents: write`. The build and test containers
receive no GitHub token. Source checkout credentials are not persisted.

## Resource measurements

Before compilation, the workflows record free disk space, remove the unused
Android, .NET, and GHC SDK directories on the disposable hosted runner, and
record disk space again. This cleanup is not part of the local container helper.
The build and test jobs restore Maven dependencies into a runner directory mounted
at `/home/ybci/.m2/repository` in the container. Each completed attempt saves the
downloaded dependencies, including after build or test failures. Project artifacts
under `org/yb` and failed-download markers are excluded. Cache keys include the
Java POM hash, with fallback to earlier EL9/Java 11 dependency caches available to
the branch. Native build outputs are not cached.

Each job first probes the `maven-s3-wagon` POM at Maven Central from inside the
container, with a 20-second timeout and no retries. HTTP 200 selects Central.
HTTP 429, HTTP 5xx, or a network connection/transfer failure selects
[Google's Maven Central mirror](https://docs.cloud.google.com/docs/buildpacks/java#google_cloud_hosted_maven_central_mirror)
for the rest of that job. Other HTTP responses and TLS verification errors fail
the check. The generated `~/.m2/settings.xml` applies to both the initial Maven
check and later Java builds and tests, and redirects only `central` when the
mirror is selected. Each new job probes Central again.

Both jobs run Maven's non-recursive `validate` goal before C++ compilation. This
loads the parent POM and its build extensions, so failures such as an HTTP 429
while downloading `maven-s3-wagon` stop the run early. It does not prefetch every
test dependency. An empty cache still needs access to the selected repository,
and caching cannot guarantee that a rate-limited download will succeed.

The `maven-central-*.txt` artifacts capture the startup probe's HTTP status,
response headers and body, curl errors, and tool versions. `maven-repository.txt`
records the selected repository. Maven failures retain their exit status; there
is no automatic retry or repository switch later in the job. A successful probe
does not guarantee that Central will remain available throughout the build.

Diagnostic artifacts retain logs for seven days, including failed runs:

- `resources.log`: host RAM, filesystem space, and container memory/CPU samples
  every 30 seconds, including image setup
- `time.txt`: GNU time measurements for the container command
- `container-image.txt`: the locally built container image ID
- build/test/smoke logs and source-test reports

The base images use version tags, and OS packages and Rust are downloaded at
image build time. This is not a fully reproducible toolchain image. Build logs
record package installation, and release diagnostics include tool versions.

## Local workflow checks

```bash
for script in build-support/deepbi/*.sh; do bash -n "$script"; done
python3 -m unittest discover -s build-support/deepbi -p 'test_*.py'
actionlint .github/workflows/deepbi-build-release.yml .github/workflows/deepbi-test.yml
git diff --check
```

These checks do not compile YugabyteDB or qualify GitHub Actions execution.
