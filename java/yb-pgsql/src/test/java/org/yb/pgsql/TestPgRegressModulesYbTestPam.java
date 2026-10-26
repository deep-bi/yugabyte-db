// Copyright (c) YugabyteDB, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"), you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.

package org.yb.pgsql;

import java.io.File;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;

import org.junit.Assume;
import org.junit.BeforeClass;
import org.junit.Test;
import org.junit.runner.RunWith;
import org.yb.YBTestRunner;
import org.yb.client.TestUtils;

@RunWith(value = YBTestRunner.class)
public class TestPgRegressModulesYbTestPam extends BasePgRegressTest {
  @BeforeClass
  public static void requirePamBuild() throws Exception {
    Path config = Paths.get(TestUtils.getBuildRootDir(), "postgres/include/server/pg_config.h");
    Assume.assumeTrue("Requires a --with-pam build", Files.readAllLines(config).stream()
        .anyMatch(line -> line.matches("#define\\s+USE_PAM\\s+1")));
  }

  @Test
  public void schedule() throws Exception {
    runPgRegressTest(new File(TestUtils.getBuildRootDir(),
        "postgres_build/src/test/modules/yb_test_pam"), "yb_schedule");
  }
}
