CREATE FUNCTION yb_test_pam_cleanup(text) RETURNS boolean
AS '$libdir/yb_test_pam', 'yb_test_pam_cleanup'
LANGUAGE C STRICT;

SET client_min_messages = error;

SELECT scenario, yb_test_pam_cleanup(scenario) AS cleaned
FROM (VALUES
  ('auth_failure'),
  ('account_failure'),
  ('auth_eof'),
  ('account_eof'),
  ('start_failure'),
  ('user_failure'),
  ('rhost_failure'),
  ('conversation_failure'),
  ('hostname_failure'),
  ('auth_error'),
  ('account_error'),
  ('ignored_auth_error'),
  ('ignored_account_error'),
  ('end_failure'),
  ('failure_and_end_failure'),
  ('eof_and_end_failure'),
  ('unknown_user'),
  ('directory_unavailable'),
  ('max_attempts'),
  ('account_expired'),
  ('password_expired'),
  ('default_service'),
  ('empty_service'),
  ('hostname_success'),
  ('success'),
  ('local_success')
) AS cases(scenario);

SELECT bool_and(yb_test_pam_cleanup(
  CASE WHEN i % 2 = 0 THEN 'success' ELSE 'auth_failure' END)) AS reusable
FROM generate_series(1, 100) AS attempts(i);

DROP FUNCTION yb_test_pam_cleanup(text);

CREATE FUNCTION yb_test_pam_conversation(text) RETURNS boolean
AS '$libdir/yb_test_pam', 'yb_test_pam_conversation'
LANGUAGE C STRICT;

SELECT scenario, yb_test_pam_conversation(scenario) AS handled
FROM (VALUES
  ('mixed_messages'),
  ('fallback_password'),
  ('password_challenge'),
  ('password_eof'),
  ('malformed_password'),
  ('empty_password'),
  ('interrupted_password'),
  ('allocation_failure'),
  ('response_failure'),
  ('zero_messages'),
  ('negative_messages'),
  ('too_many_messages'),
  ('echo_on'),
  ('unknown_style')
) AS cases(scenario);

DROP FUNCTION yb_test_pam_conversation(text);
