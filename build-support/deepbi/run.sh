#!/usr/bin/env bash
set -euo pipefail

mode=${1:?usage: run.sh build|test|smoke source_dir output_dir}
case "$mode" in
  build|test) stage=build ;;
  smoke) stage=smoke ;;
  *) echo "Unknown mode: $mode" >&2; exit 2 ;;
esac
source_dir=$(realpath "${2:?source directory required}")
mkdir -p "${3:?output directory required}"
output_dir=$(realpath "$3")
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
run_name="yb-ci-${mode}-${GITHUB_RUN_ID:-local}-${GITHUB_RUN_ATTEMPT:-1}-$$"
monitor_pid=

snapshot() {
  date -u '+%Y-%m-%dT%H:%M:%SZ'
  free -b
  df -B1 "$source_dir" "$output_dir" /var/lib/docker
  docker stats --no-stream --format '{{.Name}} {{.MemUsage}} {{.CPUPerc}}' "$run_name" \
    2>/dev/null || true
}

cleanup() {
  status=$?
  trap - EXIT INT TERM
  if [[ -n $monitor_pid ]]; then
    kill "$monitor_pid" 2>/dev/null || true
    wait "$monitor_pid" 2>/dev/null || true
  fi
  snapshot >> "$output_dir/resources.log" 2>&1 || true
  # remove only the container created by this invocation
  if docker container inspect "$run_name" >/dev/null 2>&1; then
    docker rm -f "$run_name" >/dev/null || true
  fi
  exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

(
  while true; do
    snapshot
    sleep 30
  done
) >> "$output_dir/resources.log" 2>&1 &
monitor_pid=$!

docker build --pull --target "$stage" \
  --build-arg "BUILD_UID=$(id -u)" --build-arg "BUILD_GID=$(id -g)" \
  --tag "$run_name" "$script_dir"
docker image inspect "$run_name" --format '{{.Id}}' > "$output_dir/container-image.txt"

mount_mode=rw
[[ $mode != smoke ]] || mount_mode=ro
maven_mount=()
if [[ $stage == build && -n ${MAVEN_CACHE_DIR:-} ]]; then
  mkdir -p "$MAVEN_CACHE_DIR"
  maven_mount=(-v "$(realpath "$MAVEN_CACHE_DIR"):/home/ybci/.m2/repository")
fi
docker run --rm --init --name "$run_name" --shm-size=1g \
  --ulimit core=0 --ulimit nofile=65536:65536 \
  -v "$source_dir:/src:$mount_mode" -v "$output_dir:/out" \
  "${maven_mount[@]}" \
  -e "RELEASE_TAG=${RELEASE_TAG:-}" -e "SOURCE_SHA=${SOURCE_SHA:-}" \
  "$run_name" /usr/bin/time -v -o /out/time.txt bash "/ci/$mode.sh"
