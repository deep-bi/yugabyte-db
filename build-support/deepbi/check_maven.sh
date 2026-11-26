#!/usr/bin/env bash
set -euo pipefail

output_dir=${1:?diagnostic output directory required}
mkdir -p "$output_dir"
if mvn --batch-mode --non-recursive --file java/pom.xml validate; then
  exit 0
else
  status=$?
fi

# diagnostics must not replace Maven's failure status
set +e
echo 'Maven failed; checking Maven Central with curl'
{
  date -u
  mvn --version
  curl --version
} > "$output_dir/maven-central-tools.txt" 2>&1

url=https://repo.maven.apache.org/maven2/com/github/seahen/maven-s3-wagon/1.3.3/maven-s3-wagon-1.3.3.pom
# retain HTTP error bodies and make only one bounded download attempt
curl --silent --show-error --location --connect-timeout 10 --max-time 30 \
  --dump-header "$output_dir/maven-central-headers.txt" \
  --output "$output_dir/maven-central-body.txt" \
  --write-out 'url=%{url_effective}\nhttp_code=%{http_code}\nremote_ip=%{remote_ip}\ntime_total=%{time_total}\n' \
  "$url" > "$output_dir/maven-central-transfer.txt" 2>&1
curl_status=$?
printf 'curl_exit_code=%s\n' "$curl_status" >> "$output_dir/maven-central-transfer.txt"
cat "$output_dir/maven-central-transfer.txt"
echo 'Response headers, body, and tool versions are in the maven-central-*.txt artifacts'
exit "$status"
