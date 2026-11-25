#!/usr/bin/env bash
set -euo pipefail
export PGCONNECT_TIMEOUT=3 PGSSLMODE=disable
cd /src
sha256sum --check SHA256SUMS
archives=(/src/*.tar.gz)
[[ ${#archives[@]} == 1 && -f ${archives[0]} ]]
mkdir /tmp/yb-package
tar --no-same-owner -xzf "${archives[0]}" -C /tmp/yb-package
packages=(/tmp/yb-package/yugabyte-*)
[[ ${#packages[@]} == 1 && -d ${packages[0]} ]]
cd "${packages[0]}"
python3 - <<'PY'
import json
import pathlib

manifest = json.loads(pathlib.Path('/src/build-manifest.json').read_text())
metadata = json.loads(pathlib.Path('version_metadata.json').read_text())
for key in ('git_hash', 'version_number', 'build_number'):
    assert metadata[key] == manifest['version_metadata'][key], (key, metadata)
assert metadata['git_hash'] == manifest['source_sha']
for pattern in ('yb_test_pam.so', 'yb_test_pam_provider.so'):
    assert not list(pathlib.Path('.').rglob(pattern)), f'Test library shipped: {pattern}'
PY
bash bin/post_install.sh
bin/yb-master --version
bin/yb-tserver --version
[[ -x bin/odyssey ]]

data=/tmp/yb-smoke
mkdir -p "$data" /out/daemon-logs
master_pid=
tserver_pid=
stop_process() {
  local pid=$1
  kill "$pid" 2>/dev/null || true
  for ((attempt=0; attempt<30; attempt++)); do
    if ! kill -0 "$pid" 2>/dev/null; then
      wait "$pid" 2>/dev/null || true
      return
    fi
    sleep 1
  done
  kill -KILL "$pid" 2>/dev/null || true
  wait "$pid" 2>/dev/null || true
  echo "Process $pid did not stop within 30 seconds" >&2
  return 1
}
cleanup() {
  status=$?
  trap - EXIT INT TERM
  [[ -z $tserver_pid ]] || stop_process "$tserver_pid" || true
  [[ -z $master_pid ]] || stop_process "$master_pid" || true
  exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

bin/yb-master \
  --fs_data_dirs="$data/master" --master_addresses=127.0.0.1:7100 \
  --rpc_bind_addresses=127.0.0.1:7100 --webserver_interface=127.0.0.1 \
  --webserver_port=7000 --replication_factor=1 --memory_limit_hard_bytes=1073741824 \
  --logtostderr > /out/daemon-logs/master.log 2>&1 &
master_pid=$!

admin_sql() {
  bin/ysqlsh -X -v ON_ERROR_STOP=1 -h 127.0.0.1 -p 5433 -U yugabyte -d yugabyte "$@"
}
check_pam() {
  local port=$1
  local result
  result=$(PGPASSWORD=yb-ci-smoke-password bin/ysqlsh -X -At -v ON_ERROR_STOP=1 \
    -h 127.0.0.1 -p "$port" -U ybci -d yugabyte \
    -c 'SELECT count(*) || chr(58) || sum(value) FROM workflow_smoke')
  [[ $result == 100:5150 ]]
  if PGPASSWORD=wrong-password bin/ysqlsh -X -w \
    -h 127.0.0.1 -p "$port" -U ybci -d yugabyte -c 'SELECT 1' \
    > "/out/denied-${mode}.log" 2>&1; then
    echo "Wrong PAM password accepted in $mode" >&2
    return 1
  fi
  grep -Eqi '(PAM|password) authentication failed' "/out/denied-${mode}.log"
  PGPASSWORD=yb-ci-smoke-password bin/ysqlsh -X -w -v ON_ERROR_STOP=1 \
    -h 127.0.0.1 -p "$port" -U ybci -d yugabyte -c 'SELECT current_user'
}

for mode in direct cm-backend cm-passthrough; do
  conn_mgr=false
  backend=true
  port=5433
  if [[ $mode != direct ]]; then
    conn_mgr=true
    port=6433
  fi
  [[ $mode != cm-passthrough ]] || backend=false
  bin/yb-tserver \
    --fs_data_dirs="$data/tserver" --tserver_master_addrs=127.0.0.1:7100 \
    --rpc_bind_addresses=127.0.0.1:9100 --webserver_interface=127.0.0.1 \
    --webserver_port=9000 --pgsql_proxy_bind_address=127.0.0.1:5433 \
    --memory_limit_hard_bytes=2147483648 --ysql_enable_auth=true \
    '--ysql_hba_conf_csv=host all yugabyte 127.0.0.1/32 trust,host all all 127.0.0.1/32 pam pamservice=yb-ci' \
    --enable_ysql_conn_mgr="$conn_mgr" --ysql_conn_mgr_port=6433 \
    --ysql_conn_mgr_use_auth_backend="$backend" --logtostderr \
    > "/out/daemon-logs/tserver-${mode}.log" 2>&1 &
  tserver_pid=$!
  deadline=$((SECONDS + 180))
  until admin_sql -Atc 'SELECT 1' >/dev/null 2>&1; do
    kill -0 "$master_pid"
    kill -0 "$tserver_pid"
    if (( SECONDS >= deadline )); then
      echo "YSQL did not become ready in $mode" >&2
      exit 1
    fi
    sleep 2
  done
  if [[ $mode == direct ]]; then
    admin_sql <<'SQL'
CREATE ROLE ybci LOGIN;
CREATE TABLE workflow_smoke (id int PRIMARY KEY, value int);
INSERT INTO workflow_smoke SELECT n, n FROM generate_series(1, 100) AS n;
UPDATE workflow_smoke SET value = value + 1;
GRANT SELECT ON workflow_smoke TO ybci;
SQL
  fi
  # the pooler can become ready after the direct PostgreSQL port
  until PGPASSWORD=yb-ci-smoke-password bin/ysqlsh -X -w -At \
    -h 127.0.0.1 -p "$port" -U ybci -d yugabyte -c 'SELECT 1' >/dev/null 2>&1; do
    kill -0 "$tserver_pid"
    (( SECONDS < deadline )) || exit 1
    sleep 2
  done
  check_pam "$port"
  printf '%s: SQL and PAM success, wrong-password rejection, recovery passed\n' "$mode" \
    >> /out/smoke-results.txt
  stop_process "$tserver_pid"
  tserver_pid=
done
