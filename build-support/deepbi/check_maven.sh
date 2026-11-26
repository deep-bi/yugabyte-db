#!/usr/bin/env bash
set -euo pipefail

output_dir=${1:?diagnostic output directory required}
settings_file=${2:-$HOME/.m2/settings.xml}
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
mkdir -p "$output_dir" "$(dirname -- "$settings_file")"
{
  date -u
  mvn --version
  curl --version
} > "$output_dir/maven-central-tools.txt" 2>&1

url=https://repo.maven.apache.org/maven2/com/github/seahen/maven-s3-wagon/1.3.3/maven-s3-wagon-1.3.3.pom
# choose once per job without waiting for Maven's download retries
curl_status=0
curl --silent --show-error --location --connect-timeout 5 --max-time 20 \
  --dump-header "$output_dir/maven-central-headers.txt" \
  --output "$output_dir/maven-central-body.txt" \
  --write-out 'url=%{url_effective}\nhttp_code=%{http_code}\nremote_ip=%{remote_ip}\ntime_total=%{time_total}\n' \
  "$url" > "$output_dir/maven-central-transfer.txt" \
  2> "$output_dir/maven-central-curl-error.txt" || curl_status=$?
printf 'curl_exit_code=%s\n' "$curl_status" >> "$output_dir/maven-central-transfer.txt"
cat "$output_dir/maven-central-transfer.txt"
http_code=$(sed -n 's/^http_code=//p' "$output_dir/maven-central-transfer.txt")

repository=central
case "$curl_status" in
  0)
    case "$http_code" in
      200) ;;
      429|5??) repository=google-maven-central ;;
      *) echo "Central probe failed: HTTP $http_code" >&2; exit 1 ;;
    esac
    ;;
  5|6|7|18|28|52|55|56) repository=google-maven-central ;;
  *) cat "$output_dir/maven-central-curl-error.txt" >&2; exit "$curl_status" ;;
esac

if [[ $repository == central ]]; then
  printf '%s\n' '<settings xmlns="http://maven.apache.org/SETTINGS/1.0.0"/>' > "$settings_file"
else
  cp "$script_dir/maven-settings.xml" "$settings_file"
fi
printf '%s\n' "$repository" > "$output_dir/maven-repository.txt"
echo "Using $repository for this job (Central HTTP $http_code, curl exit $curl_status)"

# later Java builds read the same settings file; Maven failures remain failures
mvn --batch-mode --non-recursive --settings "$settings_file" --file java/pom.xml validate
