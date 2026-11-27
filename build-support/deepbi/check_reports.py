#!/usr/bin/env python3
"""Reject failed, missing, or entirely skipped Java test results."""

import pathlib
import sys
import xml.etree.ElementTree as ET


def check_reports(source: pathlib.Path) -> int:
    if source.is_file():
        artifacts = map(pathlib.Path, source.read_text().splitlines())
        reports = sorted({path for path in artifacts if path.match('TEST-*.xml')})
    else:
        reports = sorted(source.glob('*/target/surefire-reports*/TEST-*.xml'))
    executed = 0
    problems = []
    for path in reports:
        try:
            root = ET.parse(path).getroot()
            for result in root.iter('testsuite'):
                if int(result.get('failures', '0')) or int(result.get('errors', '0')):
                    problems.append(f'Failed suite: {path}')
            for case in root.iter('testcase'):
                if case.find('skipped') is not None:
                    continue
                executed += 1
                if case.find('failure') is not None or case.find('error') is not None:
                    problems.append(f'Failed test: {path}: {case.get("name")}')
        except (ET.ParseError, ValueError, OSError) as error:
            problems.append(f'Invalid report {path}: {error}')
    if not executed:
        problems.append('No executed Java tests found')
    print(f'Java reports: {len(reports)}, executed tests: {executed}')
    for problem in problems:
        print(problem, file=sys.stderr)
    return int(bool(problems))


if __name__ == '__main__':
    sys.exit(check_reports(pathlib.Path(sys.argv[1])))
