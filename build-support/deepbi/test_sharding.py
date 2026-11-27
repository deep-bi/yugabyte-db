import collections
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET


SCRIPTS = pathlib.Path(__file__).parent


def container_script(name, source, output, ci):
    script = (SCRIPTS / name).read_text()
    for old, new in (('/src', source), ('/out', output), ('/ci/', str(ci) + '/')):
        script = script.replace(old, str(new))
    return script


class PreparationTest(unittest.TestCase):
    def test_cpp_prepare_stages_controller_and_preserves_java_revision(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            source = root / 'source'
            build = source / 'build'
            config = build / 'latest/postgres/include/server/pg_config.h'
            config.parent.mkdir(parents=True)
            config.write_text('#define USE_PAM 1\n')
            java_stamp = build / 'java-test-source-sha.txt'
            java_stamp.write_text('older-commit\n')
            tools = root / 'bin'
            tools.mkdir()
            git = tools / 'git'
            git.write_text('#!/bin/sh\necho current-commit\n')
            git.chmod(0o755)
            builder = source / 'yb_build.sh'
            builder.write_text('#!/bin/sh\nexit 0\n')
            builder.chmod(0o755)
            support = source / 'build-support'
            support.mkdir()
            (support / 'common-test-env.sh').write_text('''
detect_architecture() { YB_TARGET_ARCH=x86_64; }
prep_ybc_testing() {
  [[ $YB_TARGET_ARCH == x86_64 ]]
  mkdir -p build/ybc
  cp /bin/true build/ybc/yb-controller-server
  cp /bin/true build/ybc/yb-controller-cli
}
''')
            script = container_script('prepare-tests.sh', source, root / 'out', root / 'ci')
            result = subprocess.run(['bash', '-c', script, 'prepare-tests.sh', 'cpp'],
                                    env=dict(os.environ, PATH=f'{tools}:{os.environ["PATH"]}'),
                                    capture_output=True, text=True, timeout=30)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertTrue(os.access(build / 'ybc/yb-controller-server', os.X_OK))
            self.assertTrue(os.access(build / 'ybc/yb-controller-cli', os.X_OK))
            self.assertEqual((build / 'cpp-test-source-sha.txt').read_text(), 'current-commit\n')
            self.assertEqual(java_stamp.read_text(), 'older-commit\n')


@unittest.skipUnless(shutil.which('ctest'), 'CTest is required')
class ShardingTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = pathlib.Path(self.temp.name)
        self.java_tests = [f'org.yb.TestShard#method{i:02d}[parameter {i}]' for i in range(17)]

    def run_shard(self, suite='all', index=1, count=1, fail_cpp=False, fail_java=False,
                  java_tests=None, cpp_sha='fixture-source-sha', java_sha='fixture-source-sha',
                  stale_report=None, write_java_report=True, missing_maven_settings=False):
        root = pathlib.Path(tempfile.mkdtemp(dir=self.root))
        source, output, ci, tools = [root / name for name in ('source', 'out', 'ci', 'bin')]
        for directory in (source, output, ci, tools):
            directory.mkdir()
        build = source / 'build' / 'latest'
        config = build / 'postgres' / 'include' / 'server' / 'pg_config.h'
        config.parent.mkdir(parents=True)
        config.write_text('#define USE_PAM 1\n')
        (build.parent / 'cpp-test-source-sha.txt').write_text(cpp_sha + '\n')
        (build.parent / 'java-test-source-sha.txt').write_text(java_sha + '\n')
        (root / '.m2').mkdir()
        if not missing_maven_settings:
            shutil.copy(SCRIPTS / 'maven-settings.xml', build.parent)
        methods = self.java_tests if java_tests is None else java_tests
        (build / 'java_test_list.txt').write_text('\n'.join(reversed(methods + methods)) + '\n\n')
        if stale_report is not None:
            stale_dir = source / 'java/module/target/surefire-reports_previous'
            stale_dir.mkdir(parents=True)
            (stale_dir / 'TEST-Previous.xml').write_text(stale_report)
        cpp_tests = ['ann_methods_index_merge-test'] if suite == 'probe' else [
            f'case_{i:02d}' for i in range(17)]
        (build / 'CTestTestfile.cmake').write_text(''.join(
            f'add_test({name} "{sys.executable}" "-c" '
            f'"raise SystemExit({int(fail_cpp and i == 0)})")\n' for i, name in enumerate(cpp_tests)))
        git = tools / 'git'
        git.write_text('#!/bin/sh\nprintf "fixture-source-sha\\n"\n')
        git.chmod(0o755)
        shutil.copy(SCRIPTS / 'check_reports.py', ci)
        builder = source / 'yb_build.sh'
        builder.write_text('''#!/usr/bin/env python3
import json
import os
import pathlib
import shlex
import subprocess
import sys
import xml.etree.ElementTree as ET

args = sys.argv[1:]
with open('calls.jsonl', 'a') as calls:
    calls.write(json.dumps(args) + '\\n')
if '--ctest-args' in args:
    sys.exit(subprocess.call(['ctest', '--test-dir', 'build/latest', '-j4', '--verbose'] +
                            shlex.split(args[args.index('--ctest-args') + 1])))
if '--java-test' in args:
    method = args[args.index('--java-test') + 1]
    if os.environ.get('FAIL_JAVA') == method:
        sys.exit(1)
    test_class, test_method = method.split('#')
    reports = pathlib.Path('java/module/target') / f'surefire-reports_{test_class}__{test_method}'
    reports.mkdir(parents=True, exist_ok=True)
    report = reports / f'TEST-{test_class}.xml'
    if 'YB_TEST_ARTIFACT_LIST_PATH' in os.environ:
        with open(os.environ['YB_TEST_ARTIFACT_LIST_PATH'], 'a') as artifacts:
            print(report.resolve(), file=artifacts)
            print(reports / f'{test_class}*', file=artifacts)
    if os.environ['WRITE_JAVA_REPORT'] == '0':
        sys.exit(0)
    suite = ET.Element('testsuite')
    ET.SubElement(suite, 'testcase', name=method)
    ET.ElementTree(suite).write(report)
''')
        builder.chmod(0o755)
        # relocate container mount paths while exercising the actual orchestration script
        runner = root / 'test.sh'
        runner.write_text(container_script('test.sh', source, output, ci))
        env = dict(os.environ, HOME=str(root), PATH=f'{tools}:{os.environ["PATH"]}', TEST_SUITE=suite,
                   TEST_SHARD_INDEX=str(index), TEST_SHARD_COUNT=str(count),
                   FAIL_JAVA=self.java_tests[0] if fail_java else '',
                   WRITE_JAVA_REPORT=str(int(write_java_report)))
        result = subprocess.run(['bash', str(runner), '--prebuilt'], env=env, capture_output=True,
                                text=True, timeout=30)
        calls_file = source / 'calls.jsonl'
        calls = [json.loads(line) for line in calls_file.read_text().splitlines()] \
            if calls_file.exists() else []
        return result, output, calls

    def test_probe_runs_both_prebuilt_tests(self):
        method = 'org.yb.pgsql.TestPgSelect#testWhereClause'
        result, output, calls = self.run_shard('probe', java_tests=[method])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(len(calls), 2)
        self.assertIn('--skip-build', calls[0])
        self.assertEqual(calls[1][-3:], ['--java-test', method, '--skip-build'])
        self.assertEqual((output.parent / '.m2/settings.xml').read_bytes(),
                         (SCRIPTS / 'maven-settings.xml').read_bytes())
        self.assertEqual([case.get('name') for case in ET.parse(output / 'ctest.xml').iter('testcase')],
                         ['ann_methods_index_merge-test'])

    def test_cpp_probe_failure_still_runs_java(self):
        method = 'org.yb.pgsql.TestPgSelect#testWhereClause'
        result, _, calls = self.run_shard('probe', java_tests=[method], fail_cpp=True)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertEqual(calls[-1][-3:], ['--java-test', method, '--skip-build'])

    def test_java_probe_setup_failure_still_produces_cpp_result(self):
        method = 'org.yb.pgsql.TestPgSelect#testWhereClause'
        for options in ({'java_tests': []}, {'java_tests': [method], 'missing_maven_settings': True}):
            with self.subTest(options=options):
                result, output, calls = self.run_shard('probe', **options)
                self.assertNotEqual(result.returncode, 0)
                self.assertTrue((output / 'ctest.xml').is_file())
                self.assertEqual(len(calls), 1)
                self.assertIn('--ctest-args', calls[0])

    def test_wrong_source_commit_stops_before_tests(self):
        for suite, stamp in [('cpp', 'cpp_sha'), ('java', 'cpp_sha'), ('java', 'java_sha')]:
            with self.subTest(suite=suite, stamp=stamp):
                result, _, calls = self.run_shard(suite, **{stamp: 'some-other-commit'})
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(calls, [])

    def test_stale_failures_do_not_fail_the_selected_java_tests(self):
        for suite in ('java', 'probe'):
            with self.subTest(suite=suite):
                result, _, _ = self.run_shard(
                    suite, java_tests=['org.yb.pgsql.TestPgSelect#testWhereClause'],
                    stale_report='<testsuite failures="1"><testcase><failure/></testcase></testsuite>')
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_stale_success_cannot_replace_a_missing_selected_report(self):
        result, _, _ = self.run_shard(
            'java', java_tests=self.java_tests[:2], write_java_report=False,
            stale_report='<testsuite><testcase name="old"/></testsuite>')
        self.assertNotEqual(result.returncode, 0)

    def test_cpp_shards_cover_every_test_once(self):
        executed = []
        for shard in range(1, 9):
            result, output, calls = self.run_shard('cpp', shard, 8)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            executed.extend(case.get('name') for case in ET.parse(output / 'ctest.xml').iter('testcase'))
            self.assertTrue(all('--skip-build' in call for call in calls))
            self.assertFalse(any('--java-test' in call for call in calls))
        self.assertEqual(collections.Counter(executed),
                         collections.Counter(f'case_{i:02d}' for i in range(17)))

    def test_java_shards_cover_every_unique_method_once(self):
        executed = []
        for shard in range(1, 9):
            result, output, calls = self.run_shard('java', shard, 8)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            selected = (output / 'java-test-list-shard.txt').read_text().splitlines()
            self.assertEqual([call[-3:] for call in calls],
                             [['--java-test', method, '--skip-build'] for method in selected])
            self.assertFalse(any('--ctest-args' in call for call in calls))
            executed.extend(selected)
        self.assertEqual(collections.Counter(executed), collections.Counter(self.java_tests))

    def test_cpp_failure_does_not_skip_java_or_pass_the_job(self):
        result, _, calls = self.run_shard(fail_cpp=True)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertEqual(sum('--java-test' in call for call in calls), len(self.java_tests))

    def test_java_failure_does_not_skip_remaining_methods_or_pass_the_job(self):
        result, _, calls = self.run_shard('java', fail_java=True)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertEqual(sum('--java-test' in call for call in calls), len(self.java_tests))

    def test_empty_shards_fail(self):
        for suite in ('cpp', 'java'):
            with self.subTest(suite=suite):
                result, _, calls = self.run_shard(suite, 18, 18)
                self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertFalse(any('--ctest-args' in call or '--java-test' in call for call in calls))

    def test_blank_java_list_fails(self):
        result, _, _ = self.run_shard('java', java_tests=[])
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_invalid_selection_fails_before_building(self):
        for suite, index, count in [('typo', 1, 8), ('cpp', 0, 8), ('cpp', 9, 8),
                                    ('java', 1, 0), ('java', 'x', 8), ('java', 1, '08')]:
            with self.subTest(suite=suite, index=index, count=count):
                result, _, calls = self.run_shard(suite, index, count)
                self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                self.assertEqual(calls, [])


if __name__ == '__main__':
    unittest.main()
