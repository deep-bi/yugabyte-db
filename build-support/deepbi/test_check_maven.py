import os
import pathlib
import subprocess
import tempfile
import unittest
import xml.etree.ElementTree as ET


SCRIPT = pathlib.Path(__file__).with_name('check_maven.sh')
NAMESPACE = {'m': 'http://maven.apache.org/SETTINGS/1.0.0'}


class CheckMavenTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = pathlib.Path(self.temp.name)
        self.settings = self.root / 'maven settings.xml'
        self.output = self.root / 'out'
        self.calls = self.root / 'mvn-calls.txt'
        self.tools = self.root / 'bin'
        self.tools.mkdir()
        self.tool('curl', '''#!/usr/bin/env bash
if [[ $1 == --version ]]; then echo 'curl fixture'; exit 0; fi
printf '%s\\n' "$*" > "$CHECK_MAVEN_TEST_ROOT/curl-call.txt"
while (( $# )); do
  case "$1" in
    --dump-header) printf 'HTTP/2 %s\\n' "$CHECK_MAVEN_HTTP_CODE" > "$2"; shift 2 ;;
    --output) printf 'probe body\\n' > "$2"; shift 2 ;;
    *) shift ;;
  esac
done
printf 'http_code=%s\\n' "$CHECK_MAVEN_HTTP_CODE"
exit "$CHECK_MAVEN_CURL_STATUS"
''')
        self.tool('mvn', '''#!/usr/bin/env bash
if [[ $1 == --version ]]; then echo 'Maven fixture'; exit 0; fi
printf '%s\\n' "$*" >> "$CHECK_MAVEN_TEST_ROOT/mvn-calls.txt"
exit "$CHECK_MAVEN_MVN_STATUS"
''')

    def tool(self, name, content):
        path = self.tools / name
        path.write_text(content)
        path.chmod(0o755)

    def run_check(self, http=200, curl_status=0, maven_status=0):
        self.calls.write_text('')
        env = dict(os.environ, PATH=f'{self.tools}:{os.environ["PATH"]}',
                   CHECK_MAVEN_TEST_ROOT=str(self.root), CHECK_MAVEN_HTTP_CODE=str(http),
                   CHECK_MAVEN_CURL_STATUS=str(curl_status),
                   CHECK_MAVEN_MVN_STATUS=str(maven_status))
        return subprocess.run(['bash', str(SCRIPT), str(self.output), str(self.settings)],
                              env=env, capture_output=True, text=True, timeout=10)

    def assert_repository(self, expected):
        self.assertEqual((self.output / 'maven-repository.txt').read_text().strip(), expected)
        root = ET.parse(self.settings).getroot()
        mirrors = root.findall('m:mirrors/m:mirror', NAMESPACE)
        if expected == 'central':
            self.assertEqual(mirrors, [])
        else:
            self.assertEqual(len(mirrors), 1)
            self.assertEqual(mirrors[0].findtext('m:mirrorOf', namespaces=NAMESPACE), 'central')
            self.assertEqual(mirrors[0].findtext('m:url', namespaces=NAMESPACE),
                             'https://maven-central.storage-download.googleapis.com/maven2/')
        calls = self.calls.read_text().splitlines()
        self.assertEqual(len(calls), 1)
        self.assertIn(f'--settings {self.settings}', calls[0])

    def test_healthy_central_is_used(self):
        result = self.run_check()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assert_repository('central')
        probe = (self.root / 'curl-call.txt').read_text()
        self.assertIn('--connect-timeout 5', probe)
        self.assertIn('--max-time 20', probe)
        self.assertNotIn('--retry', probe)

    def test_rate_limit_and_server_errors_select_mirror(self):
        for status in (429, 500, 503):
            with self.subTest(status=status):
                result = self.run_check(http=status)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assert_repository('google-maven-central')

    def test_network_failures_select_mirror(self):
        for status in (6, 7, 28, 56):
            with self.subTest(status=status):
                result = self.run_check(http='000', curl_status=status)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assert_repository('google-maven-central')

    def test_other_http_errors_fail_without_running_maven(self):
        for status in (403, 404):
            with self.subTest(status=status):
                result = self.run_check(http=status)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(self.calls.read_text(), '')
                self.assertFalse(self.settings.exists())

    def test_certificate_and_local_errors_fail_without_running_maven(self):
        for status in (60, 23):
            with self.subTest(status=status):
                result = self.run_check(http='000', curl_status=status)
                self.assertEqual(result.returncode, status)
                self.assertEqual(self.calls.read_text(), '')
                self.assertFalse(self.settings.exists())

    def test_maven_failure_is_preserved_without_retry(self):
        for http, repository in ((200, 'central'), (429, 'google-maven-central')):
            with self.subTest(http=http):
                result = self.run_check(http=http, maven_status=17)
                self.assertEqual(result.returncode, 17)
                self.assert_repository(repository)

    def test_next_check_can_select_central_again(self):
        self.assertEqual(self.run_check(http=429).returncode, 0)
        self.assert_repository('google-maven-central')
        self.assertEqual(self.run_check(http=200).returncode, 0)
        self.assert_repository('central')


if __name__ == '__main__':
    unittest.main()
