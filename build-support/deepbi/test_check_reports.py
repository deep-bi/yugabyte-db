import contextlib
import io
import pathlib
import tempfile
import unittest

from check_reports import check_reports


class CheckReportsTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = pathlib.Path(self.temp.name)

    def report(self, name, content):
        path = self.root / 'yb-pgsql' / 'target' / f'surefire-reports_{name}' / f'TEST-{name}.xml'
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)

    def check(self):
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            return check_reports(self.root)

    def test_missing_reports_fail(self):
        self.assertEqual(self.check(), 1)

    def test_failure_even_with_successful_tests(self):
        self.report('mixed', '<testsuite><testcase classname="A" name="ok"/>'
                    '<testcase classname="A" name="bad"><failure/></testcase></testsuite>')
        self.assertEqual(self.check(), 1)

    def test_suite_error_without_failed_case(self):
        self.report('error', '<testsuite errors="1"><testcase name="ok"/></testsuite>')
        self.assertEqual(self.check(), 1)

    def test_malformed_report_fails(self):
        self.report('broken', '<testsuite')
        self.assertEqual(self.check(), 1)

    def test_skipped_test_with_successful_test_passes(self):
        self.report('mixed', '<testsuite><testcase name="ok"/>'
                    '<testcase name="skipped"><skipped/></testcase></testsuite>')
        self.assertEqual(self.check(), 0)

    def test_successful_tests_pass(self):
        self.report('success', '<testsuite><testcase classname="A" name="ok"/></testsuite>')
        self.assertEqual(self.check(), 0)

    def test_entirely_skipped_suite_fails(self):
        self.report('skipped', '<testsuite><testcase><skipped/></testcase></testsuite>')
        self.assertEqual(self.check(), 1)

    def test_nested_suites(self):
        self.report('nested', '<testsuites><testsuite><testcase name="ok"/>'
                    '</testsuite></testsuites>')
        self.assertEqual(self.check(), 0)


if __name__ == '__main__':
    unittest.main()
