// Copyright (c) YugabyteDB, Inc.

package org.yb.ysqlconnmgr;

import static org.yb.AssertionWrappers.*;
import static org.yb.ysqlconnmgr.PgWireProtocol.*;

import com.google.common.net.HostAndPort;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.EOFException;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.sql.Connection;
import java.sql.ResultSet;
import java.sql.SQLException;
import java.sql.Statement;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.TimeUnit;
import org.junit.After;
import org.junit.Before;
import org.junit.BeforeClass;
import org.junit.Assume;
import org.junit.Test;
import org.yb.client.TestUtils;
import org.yb.minicluster.MiniYBClusterBuilder;
import org.yb.pgsql.ConnectionBuilder;
import org.yb.pgsql.ConnectionEndpoint;

/** Exercises real authentication with a process-local PAM double, without system PAM changes */
public abstract class BasePamAuth extends BaseYsqlConnMgr {
  private static final String HBA =
      "host all yugabyte all trust,host all all all pam pamservice=yb-test-pam";
  private static final String[] USERS = {"pam_alice", "pam_bob", "pam_alice@example.com",
      "pam_unknown", "pam_unavailable", "pam_denied", "pam_expired", "pam_password_expired",
      "pam_messages"};
  private Path auditLog;

  @BeforeClass
  public static void requirePamBuild() throws Exception {
    Path config = Paths.get(TestUtils.getBuildRootDir(), "postgres/include/server/pg_config.h");
    Assume.assumeTrue("Requires a --with-pam build", Files.readAllLines(config).stream()
        .anyMatch(line -> line.matches("#define\\s+USE_PAM\\s+1")));
  }

  protected abstract boolean useAuthBackend();

  @Override
  protected int getInitialNumTServers() {
    return 1;
  }

  @Override
  protected int getInitialNumMasters() {
    return 1;
  }

  @Override
  protected void customizeMiniClusterBuilder(MiniYBClusterBuilder builder) {
    super.customizeMiniClusterBuilder(builder);
    builder.numTservers(1).replicationFactor(1);
    disableWarmupRandomMode(builder);
    builder.addCommonTServerFlag("ysql_hba_conf_csv", HBA);
    builder.addCommonTServerFlag("ysql_conn_mgr_use_auth_backend",
        Boolean.toString(useAuthBackend()));
    builder.addCommonTServerFlag("ysql_conn_mgr_control_connection_pool_size", "1");
    builder.addCommonTServerFlag("ysql_conn_mgr_num_workers", "1");
    builder.addCommonTServerFlag("ysql_conn_mgr_auth_msg_timeout", "3000");
    Path provider = Paths.get(TestUtils.getBuildRootDir(), "postgres_build/src/test/modules",
        "yb_test_pam_provider/yb_test_pam_provider.so");
    assertTrue("Build the PAM test provider first: " + provider, Files.isRegularFile(provider));
    auditLog = Paths.get(TestUtils.getBaseTmpDir(), getClass().getSimpleName() + "-pam.log");
    Map<String, String> env = new HashMap<>();
    String preload = System.getenv("LD_PRELOAD");
    env.put("LD_PRELOAD", (preload == null || preload.isEmpty() ? "" : preload + ":") + provider);
    env.put("YB_TEST_PAM_LOG", auditLog.toString());
    builder.addEnvironmentVariables(env);
  }

  @Before
  public void createPamRoles() throws Exception {
    Files.deleteIfExists(auditLog);
    try (Connection conn = getConnectionBuilder()
             .withConnectionEndpoint(ConnectionEndpoint.POSTGRES).connect();
         Statement stmt = conn.createStatement()) {
      for (String user : USERS) {
        // No database passwords: a successful connection must have gone through PAM
        stmt.execute("CREATE ROLE \"" + user + "\" LOGIN");
      }
    }
  }

  @After
  public void recreatePamCluster() {
    markClusterNeedsRecreation();
  }

  private ConnectionBuilder login(ConnectionEndpoint endpoint, String user, String password) {
    ConnectionBuilder builder = getConnectionBuilder().withConnectionEndpoint(endpoint)
        .withUser(user).withPassword(password).withSslMode("disable");
    builder.setMaxConnectionAttempts(1);
    return builder;
  }

  private void assertIdentity(ConnectionEndpoint endpoint, String user) throws Exception {
    try (Connection conn = login(endpoint, user, user + "_token").connect();
         Statement stmt = conn.createStatement();
         ResultSet rs = stmt.executeQuery("SELECT current_user, session_user")) {
      assertTrue(rs.next());
      assertEquals(user, rs.getString(1));
      assertEquals(user, rs.getString(2));
    }
  }

  private void assertPamRejected(ConnectionEndpoint endpoint, String user, String password)
      throws Exception {
    try (Connection conn = login(endpoint, user, password).connect()) {
      fail("PAM should reject " + user);
    } catch (SQLException e) {
      assertEquals(e.getMessage(), "28000", e.getSQLState());
      assertTrue(e.getMessage(), e.getMessage().contains("PAM authentication failed"));
    }
  }

  private List<String> auditLines() throws Exception {
    return Files.exists(auditLog) ? Files.readAllLines(auditLog) : new ArrayList<>();
  }

  @Test
  public void testIdentitiesAndClientAddress() throws Exception {
    for (ConnectionEndpoint endpoint : ConnectionEndpoint.values()) {
      for (String user : new String[]{"pam_alice", "pam_bob", "pam_alice@example.com"}) {
        assertIdentity(endpoint, user);
      }
    }
    List<String> events = auditLines();
    assertEquals(6, events.size());
    for (String event : events) {
      String[] fields = event.split("\\|");
      assertEquals(event, "127.0.0.1", fields[2]);
      assertEquals(event, "0", fields[3]);
    }
  }

  @Test
  public void testRejectionsAndRecovery() throws Exception {
    for (ConnectionEndpoint endpoint : ConnectionEndpoint.values()) {
      for (String user : new String[]{"pam_alice", "pam_unknown", "pam_unavailable",
          "pam_denied", "pam_expired", "pam_password_expired"}) {
        assertPamRejected(endpoint, user,
            user.equals("pam_alice") ? "wrong_token" : user + "_token");
        assertIdentity(endpoint, "pam_bob");
      }
      try (Connection conn = login(endpoint, "pam_missing_role", "pam_missing_role_token")
          .connect()) {
        fail("PAM success must not create a database role");
      } catch (SQLException e) {
        assertTrue(e.getMessage(),
            e.getMessage().contains("role \"pam_missing_role\" does not exist"));
      }
      assertIdentity(endpoint, "pam_alice");
    }
  }

  @Test
  public void testFailedAuthenticationReusesBackend() throws Exception {
    for (int i = 0; i < 10; i++) {
      assertPamRejected(ConnectionEndpoint.YSQL_CONN_MGR, "pam_alice", "wrong_token");
      assertIdentity(ConnectionEndpoint.YSQL_CONN_MGR, "pam_bob");
    }
    List<String> events = auditLines();
    assertEquals("Each authentication must release its PAM handle", 20, events.size());
    Set<String> pids = new HashSet<>();
    for (int i = 0; i < events.size(); i++) {
      String[] fields = events.get(i).split("\\|");
      pids.add(fields[0]);
      assertEquals(i % 2 == 0 ? "pam_alice" : "pam_bob", fields[1]);
      assertEquals(i % 2 != 0, fields[3].equals("0"));
    }
    if (!useAuthBackend()) {
      assertEquals("Failures and successes must reuse the same control backend", 1, pids.size());
    }
  }

  private Socket passwordChallenge() throws Exception {
    InetSocketAddress addr = miniCluster.getYsqlConnMgrContactPoints().get(0);
    Socket socket = new Socket();
    try {
      socket.connect(addr, 10000);
      socket.setSoTimeout(20000);
      socket.getOutputStream().write(buildStartupMessage("pam_alice", "yugabyte", null));
      PgMessage message = readMessage(new DataInputStream(socket.getInputStream()));
      assertEquals('R', message.type);
      assertEquals(3, ByteBuffer.wrap(message.body).getInt());
      return socket;
    } catch (Throwable e) {
      socket.close();
      throw e;
    }
  }

  @Test
  public void testDisconnectMalformedPasswordAndTimeout() throws Exception {
    for (int scenario = 0; scenario < 3; scenario++) {
      int before = auditLines().size();
      try (Socket socket = passwordChallenge()) {
        if (scenario == 1) {
          DataOutputStream out = new DataOutputStream(socket.getOutputStream());
          out.writeByte('p');
          out.writeInt(4); // Missing the required NUL-terminated password
          out.flush();
        }
        if (scenario != 0) {
          try {
            PgMessage message = readMessage(new DataInputStream(socket.getInputStream()));
            assertEquals("Incomplete authentication must fail", 'E', message.type);
          } catch (EOFException expected) {
            // The timeout/disconnect path may close without an ErrorResponse
          }
        }
      }
      TestUtils.waitFor(() -> auditLines().size() == before + 1, 20000);
      String event = auditLines().get(before);
      assertFalse(event, event.endsWith("|0"));
      assertIdentity(ConnectionEndpoint.YSQL_CONN_MGR, "pam_bob");
    }
  }

  @Test
  public void testPamMessagesWithLibpq() throws Exception {
    InetSocketAddress addr = miniCluster.getYsqlConnMgrContactPoints().get(0);
    for (boolean valid : new boolean[]{true, false}) {
      Path output = Files.createTempFile(
          Paths.get(TestUtils.getBaseTmpDir()), "pam-ysqlsh-", ".log");
      ProcessBuilder command = new ProcessBuilder(TestUtils.findBinary("../postgres/bin/ysqlsh"),
          "-X", "-w", "-h", addr.getHostString(), "-p", Integer.toString(addr.getPort()),
          "-U", "pam_messages", "-d", "yugabyte", "-Atc", "SELECT current_user");
      command.environment().put("PGPASSWORD", valid ? "pam_messages_token" : "wrong_token");
      command.environment().put("PGSSLMODE", "disable");
      command.redirectErrorStream(true).redirectOutput(output.toFile());
      Process process = command.start();
      try {
        assertTrue("ysqlsh hung during authentication", process.waitFor(30, TimeUnit.SECONDS));
        String text = new String(Files.readAllBytes(output), StandardCharsets.UTF_8);
        if (valid) {
          assertEquals(text, 0, process.exitValue());
          assertEquals("pam_messages", text.trim());
        } else {
          assertTrue(text, process.exitValue() != 0);
          assertTrue(text, text.contains("PAM authentication failed"));
        }
      } finally {
        process.destroyForcibly();
        process.waitFor();
      }
    }
  }

  @Test
  public void testConcurrentIdentityIsolation() throws Exception {
    ExecutorService executor = Executors.newFixedThreadPool(4);
    try {
      List<Future<?>> results = new ArrayList<>();
      for (int worker = 0; worker < 4; worker++) {
        String user = worker % 2 == 0 ? "pam_alice" : "pam_bob";
        results.add(executor.submit(() -> {
          for (int i = 0; i < 5; i++) {
            assertIdentity(ConnectionEndpoint.YSQL_CONN_MGR, user);
          }
          return null;
        }));
      }
      for (Future<?> result : results) {
        result.get(60, TimeUnit.SECONDS);
      }
      assertEquals(20, auditLines().size());
    } finally {
      executor.shutdownNow();
      assertTrue(executor.awaitTermination(30, TimeUnit.SECONDS));
    }
  }

  @Test
  public void testPamServiceReload() throws Exception {
    assertIdentity(ConnectionEndpoint.YSQL_CONN_MGR, "pam_alice");
    try {
      for (HostAndPort server : miniCluster.getTabletServers().keySet()) {
        setServerFlag(server, "ysql_hba_conf_csv", HBA.replace("yb-test-pam", "yb-test-pam-deny"));
      }
      TestUtils.waitFor(() -> {
        try (Connection conn = login(ConnectionEndpoint.YSQL_CONN_MGR,
            "pam_alice", "pam_alice_token").connect()) {
          return false;
        } catch (SQLException e) {
          assertTrue(e.getMessage(), e.getMessage().contains("PAM authentication failed"));
          return true;
        }
      }, 30000);
    } finally {
      for (HostAndPort server : miniCluster.getTabletServers().keySet()) {
        setServerFlag(server, "ysql_hba_conf_csv", HBA);
      }
    }
    TestUtils.waitFor(() -> {
      try {
        assertIdentity(ConnectionEndpoint.YSQL_CONN_MGR, "pam_alice");
        return true;
      } catch (SQLException e) {
        assertTrue(e.getMessage(), e.getMessage().contains("PAM authentication failed"));
        return false;
      }
    }, 30000);
  }
}
