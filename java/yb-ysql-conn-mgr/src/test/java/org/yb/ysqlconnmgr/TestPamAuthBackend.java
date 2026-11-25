// Copyright (c) YugabyteDB, Inc.

package org.yb.ysqlconnmgr;

import org.junit.runner.RunWith;

@RunWith(YBTestRunnerYsqlConnMgr.class)
public class TestPamAuthBackend extends BasePamAuth {
  @Override
  protected boolean useAuthBackend() {
    return true;
  }
}
