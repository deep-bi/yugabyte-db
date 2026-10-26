// Copyright (c) YugabyteDB, Inc.

package org.yb.ysqlconnmgr;

import org.junit.runner.RunWith;
import org.yb.YBTestRunner;
import org.yb.util.RequiresLinux;

@RequiresLinux
@RunWith(YBTestRunner.class)
public class TestPamAuthPassthrough extends BasePamAuth {
  @Override
  protected boolean useAuthBackend() {
    return false;
  }
}
