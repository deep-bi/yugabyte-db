/*-------------------------------------------------------------------------
 *
 * yb_test_pam.c
 *    Unit tests for PAM resource ownership in reused authentication backends
 *
 * Copyright (c) YugabyteDB, Inc.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "fmgr.h"
#include "libpq/libpq.h"

static void *yb_test_calloc(size_t count, size_t size);
static char *yb_test_strdup(const char *value);
static void yb_test_free(void *pointer);
static void yb_test_startmsgread(void);
static int yb_test_getbyte(void);
static int yb_test_getmessage(StringInfo buffer, int maxlen);
static void yb_test_endmessage(StringInfo buffer);
static int yb_test_flush(void);

/*
 * Compile the production authentication code with PAM test doubles. Renaming
 * its public symbols keeps loading this module from replacing backend auth.
 */
#define ClientAuthentication yb_test_ClientAuthentication
#define ClientAuthentication_hook yb_test_ClientAuthentication_hook
#define sendAuthRequest yb_test_sendAuthRequest
#define pg_krb_server_keyfile yb_test_pg_krb_server_keyfile
#define pg_krb_caseins_users yb_test_pg_krb_caseins_users
#define pam_start yb_test_pam_start
#define pam_set_item yb_test_pam_set_item
#define pam_authenticate yb_test_pam_authenticate
#define pam_acct_mgmt yb_test_pam_acct_mgmt
#define pam_end yb_test_pam_end
#define pam_strerror yb_test_pam_strerror
#define pg_getnameinfo_all yb_test_getnameinfo_all
#define calloc yb_test_calloc
#define strdup yb_test_strdup
#define free yb_test_free
#define pq_startmsgread yb_test_startmsgread
#define pq_getbyte yb_test_getbyte
#define pq_getmessage yb_test_getmessage
#define pq_endmessage yb_test_endmessage
#undef pq_flush
#define pq_flush() yb_test_flush()
#include "../../../backend/libpq/auth.c"
#undef calloc
#undef strdup
#undef free

PG_MODULE_MAGIC;
PG_FUNCTION_INFO_V1(yb_test_pam_cleanup);
PG_FUNCTION_INFO_V1(yb_test_pam_conversation);

#ifdef USE_PAM
static const char *yb_test_case;
static char yb_test_handle;
static int yb_test_start_calls;
static int yb_test_end_calls;
static int yb_test_end_status;
static bool yb_test_handle_active;
static bool yb_test_module_active;
static int yb_test_auth_calls;
static int yb_test_account_calls;
static int yb_test_host_calls;
static void *yb_test_allocations[PAM_MAX_NUM_MSG + 1];
static int yb_test_allocation_count;
static int yb_test_challenges;

static bool
yb_pam_case_is(const char *name)
{
	return strcmp(yb_test_case, name) == 0;
}

static void
yb_pam_check(bool condition, const char *message)
{
	if (!condition)
		ereport(ERROR,
				(errmsg("PAM cleanup test %s: %s", yb_test_case, message)));
}

static void *
yb_test_record_allocation(void *pointer)
{
	if (pointer)
	{
		yb_pam_check(yb_test_allocation_count < lengthof(yb_test_allocations),
					 "too many conversation allocations");
		yb_test_allocations[yb_test_allocation_count++] = pointer;
	}
	return pointer;
}

static void *
yb_test_calloc(size_t count, size_t size)
{
	if (yb_pam_case_is("allocation_failure"))
		return NULL;
	return yb_test_record_allocation(calloc(count, size));
}

static char *
yb_test_strdup(const char *value)
{
	if (yb_pam_case_is("response_failure") && yb_test_allocation_count == 2)
		return NULL;
	return yb_test_record_allocation(strdup(value));
}

static void
yb_test_free(void *pointer)
{
	if (!pointer)
		return;
	for (int i = 0; i < yb_test_allocation_count; i++)
	{
		if (yb_test_allocations[i] == pointer)
		{
			yb_test_allocations[i] = yb_test_allocations[--yb_test_allocation_count];
			free(pointer);
			return;
		}
	}
	yb_pam_check(false, "free of unowned memory");
}

static void
yb_test_startmsgread(void)
{
}

static int
yb_test_getbyte(void)
{
	return yb_pam_case_is("password_eof") ? EOF : 'p';
}

static int
yb_test_getmessage(StringInfo buffer, int maxlen)
{
	if (yb_pam_case_is("interrupted_password") || yb_pam_case_is("auth_error") ||
		yb_pam_case_is("account_error") || yb_pam_case_is("ignored_auth_error") ||
		yb_pam_case_is("ignored_account_error"))
		ereport(ERROR, (errcode(ERRCODE_QUERY_CANCELED), errmsg("injected password interruption")));
	if (!yb_pam_case_is("malformed_password"))
		appendBinaryStringInfo(buffer,
							   yb_pam_case_is("empty_password") ? "" : "dummy_token",
							   yb_pam_case_is("empty_password") ? 1 : sizeof("dummy_token"));
	return 0;
}

static void
yb_test_endmessage(StringInfo buffer)
{
	yb_test_challenges++;
	pfree(buffer->data);
}

static int
yb_test_flush(void)
{
	return 0;
}

int
yb_test_pam_start(const char *service, const char *user,
				  const struct pam_conv *conv, pam_handle_t **handle)
{
	yb_test_start_calls++;
	yb_pam_check(strcmp(service, (yb_pam_case_is("default_service") ||
								 yb_pam_case_is("empty_service")) ?
								 PGSQL_PAM_SERVICE : "yb-unit-test") == 0,
				 "wrong PAM service selected");
	yb_pam_check(conv->conv == pam_passwd_conv_proc, "wrong conversation callback");
	if (yb_pam_case_is("start_failure"))
		return PAM_SYSTEM_ERR;
	yb_pam_check(!yb_test_handle_active, "previous handle is still active");
	yb_test_handle_active = true;
	*handle = (pam_handle_t *) &yb_test_handle;
	return PAM_SUCCESS;
}

int
yb_test_pam_set_item(pam_handle_t *handle, int item, const void *value)
{
	yb_pam_check(handle == (pam_handle_t *) &yb_test_handle && yb_test_handle_active,
				 "setting an item on an inactive handle");
	if (item == PAM_USER)
		yb_pam_check(strcmp(value, "alice@example.com") == 0, "username changed");
	if (item == PAM_RHOST)
		yb_pam_check(strcmp(value, "192.0.2.1") == 0, "wrong remote host");
	if ((item == PAM_USER && yb_pam_case_is("user_failure")) ||
		(item == PAM_RHOST && yb_pam_case_is("rhost_failure")) ||
		(item == PAM_CONV && yb_pam_case_is("conversation_failure")))
		return PAM_SYSTEM_ERR;
	return PAM_SUCCESS;
}

int
yb_test_getnameinfo_all(const struct sockaddr_storage *addr, int salen,
					   char *node, int nodelen, char *service, int servicelen,
					   int flags)
{
	yb_test_host_calls++;
	yb_pam_check(flags == (yb_pam_case_is("hostname_success") ? 0 :
							  NI_NUMERICHOST | NI_NUMERICSERV), "wrong hostname lookup flags");
	if (yb_pam_case_is("hostname_failure"))
		return EAI_FAIL;
	strlcpy(node, "192.0.2.1", nodelen);
	return 0;
}

static int
yb_test_module_conversation(void)
{
	struct pam_message messages[] = {
		{PAM_TEXT_INFO, "test information"},
		{PAM_ERROR_MSG, "test diagnostic"},
		{PAM_PROMPT_ECHO_OFF, "Password:"}
	};
	const struct pam_message *pointers[] = {&messages[0], &messages[1], &messages[2]};
	struct pam_response *responses = NULL;
	int			status;

	/* Linux-PAM rejects pam_end until module dispatch has returned normally */
	yb_test_module_active = true;
	status = pam_passwd_conv_proc(lengthof(messages), pointers, &responses, "");
	yb_pam_check(responses == NULL, "failed conversation exposed responses");
	if (yb_pam_case_is("ignored_auth_error") || yb_pam_case_is("ignored_account_error"))
	{
		/* An optional module can retry or ignore a failed conversation */
		status = pam_passwd_conv_proc(lengthof(messages), pointers, &responses, "");
		yb_pam_check(status == PAM_CONV_ERR && responses == NULL,
					 "conversation retry accepted after an error");
		status = PAM_SUCCESS;
	}
	yb_test_module_active = false;
	return status;
}

int
yb_test_pam_authenticate(pam_handle_t *handle, int flags)
{
	yb_test_auth_calls++;
	if (yb_pam_case_is("auth_error") || yb_pam_case_is("ignored_auth_error") ||
		yb_pam_case_is("malformed_password") || yb_pam_case_is("empty_password") ||
		yb_pam_case_is("interrupted_password"))
		return yb_test_module_conversation();
	if (yb_pam_case_is("auth_eof") || yb_pam_case_is("eof_and_end_failure"))
	{
		pam_no_password = true;
		return PAM_CONV_ERR;
	}
	if (yb_pam_case_is("auth_failure") || yb_pam_case_is("failure_and_end_failure"))
		return PAM_AUTH_ERR;
	if (yb_pam_case_is("unknown_user"))
		return PAM_USER_UNKNOWN;
	if (yb_pam_case_is("directory_unavailable"))
		return PAM_AUTHINFO_UNAVAIL;
	if (yb_pam_case_is("max_attempts"))
		return PAM_MAXTRIES;
	return PAM_SUCCESS;
}

int
yb_test_pam_acct_mgmt(pam_handle_t *handle, int flags)
{
	yb_test_account_calls++;
	if (yb_pam_case_is("account_error") || yb_pam_case_is("ignored_account_error"))
		return yb_test_module_conversation();
	if (yb_pam_case_is("account_eof"))
	{
		pam_no_password = true;
		return PAM_CONV_ERR;
	}
	if (yb_pam_case_is("account_failure"))
		return PAM_PERM_DENIED;
	if (yb_pam_case_is("account_expired"))
		return PAM_ACCT_EXPIRED;
	if (yb_pam_case_is("password_expired"))
		return PAM_NEW_AUTHTOK_REQD;
	return PAM_SUCCESS;
}

int
yb_test_pam_end(pam_handle_t *handle, int status)
{
	yb_pam_check(handle == (pam_handle_t *) &yb_test_handle && yb_test_handle_active,
				 "invalid handle or double cleanup");
	yb_test_end_calls++;
	yb_test_end_status = status;
	if (yb_test_module_active)
		return PAM_SYSTEM_ERR;
	yb_test_handle_active = false;
	if (yb_pam_case_is("end_failure") || yb_pam_case_is("failure_and_end_failure") ||
		yb_pam_case_is("eof_and_end_failure"))
		return PAM_SYSTEM_ERR;
	return PAM_SUCCESS;
}

const char *
yb_test_pam_strerror(pam_handle_t *handle, int status)
{
	return "injected PAM failure";
}
#endif

Datum
yb_test_pam_cleanup(PG_FUNCTION_ARGS)
{
#ifdef USE_PAM
	Port		port = {0};
	HbaLine		hba = {0};
	MemoryContext oldcontext = CurrentMemoryContext;
	volatile int result = STATUS_ERROR;
	volatile bool interrupted = false;
	int			expected_status = PAM_SYSTEM_ERR;
	int			expected_result = STATUS_ERROR;
	bool		expect_interrupt;

	yb_test_case = text_to_cstring(PG_GETARG_TEXT_PP(0));
	yb_test_start_calls = 0;
	yb_test_end_calls = 0;
	yb_test_end_status = -1;
	yb_test_handle_active = false;
	yb_test_module_active = false;
	yb_test_allocation_count = 0;
	yb_test_challenges = 0;
	yb_test_auth_calls = 0;
	yb_test_account_calls = 0;
	yb_test_host_calls = 0;
	hba.conntype = yb_pam_case_is("local_success") ? ctLocal : ctHost;
	hba.auth_method = uaPAM;
	hba.pamservice = yb_pam_case_is("default_service") ? NULL :
		yb_pam_case_is("empty_service") ? "" : "yb-unit-test";
	hba.pam_use_hostname = yb_pam_case_is("hostname_success");
	port.hba = &hba;
	port.raddr.addr.ss_family = AF_INET;
	port.raddr.salen = sizeof(struct sockaddr_in);

	expect_interrupt = yb_pam_case_is("auth_error") || yb_pam_case_is("account_error") ||
		yb_pam_case_is("ignored_auth_error") || yb_pam_case_is("ignored_account_error");
	if (expect_interrupt)
		expected_status = PAM_ABORT;
	else if (yb_pam_case_is("auth_failure") || yb_pam_case_is("failure_and_end_failure"))
		expected_status = PAM_AUTH_ERR;
	else if (yb_pam_case_is("account_failure"))
		expected_status = PAM_PERM_DENIED;
	else if (yb_pam_case_is("unknown_user"))
		expected_status = PAM_USER_UNKNOWN;
	else if (yb_pam_case_is("directory_unavailable"))
		expected_status = PAM_AUTHINFO_UNAVAIL;
	else if (yb_pam_case_is("max_attempts"))
		expected_status = PAM_MAXTRIES;
	else if (yb_pam_case_is("account_expired"))
		expected_status = PAM_ACCT_EXPIRED;
	else if (yb_pam_case_is("password_expired"))
		expected_status = PAM_NEW_AUTHTOK_REQD;
	else if (yb_pam_case_is("auth_eof") || yb_pam_case_is("account_eof") ||
			 yb_pam_case_is("eof_and_end_failure"))
	{
		expected_status = PAM_CONV_ERR;
		expected_result = STATUS_EOF;
	}
	else if (yb_pam_case_is("success") || yb_pam_case_is("local_success") ||
			 yb_pam_case_is("default_service") || yb_pam_case_is("empty_service") ||
			 yb_pam_case_is("hostname_success"))
	{
		expected_status = PAM_SUCCESS;
		expected_result = STATUS_OK;
	}
	else if (yb_pam_case_is("end_failure"))
		expected_status = PAM_SUCCESS;
	else
		yb_pam_check(yb_pam_case_is("start_failure") ||
					 yb_pam_case_is("user_failure") ||
					 yb_pam_case_is("rhost_failure") ||
					 yb_pam_case_is("conversation_failure") ||
					 yb_pam_case_is("hostname_failure"), "unknown test case");

	PG_TRY();
	{
		result = CheckPAMAuth(&port, "alice@example.com", "dummy_token");
	}
	PG_CATCH();
	{
		MemoryContextSwitchTo(oldcontext);
		if (!expect_interrupt || geterrcode() != ERRCODE_QUERY_CANCELED)
			PG_RE_THROW();
		FlushErrorState();
		interrupted = true;
	}
	PG_END_TRY();

	yb_pam_check(yb_test_start_calls == 1, "pam_start must be called once");
	yb_pam_check(yb_test_end_calls == (yb_pam_case_is("start_failure") ? 0 : 1),
				 "pam_end must be called once for every successfully started handle");
	yb_pam_check(!yb_test_handle_active, "PAM handle leaked");
	yb_pam_check(yb_test_allocation_count == 0, "conversation memory leaked");
	if (expect_interrupt)
		yb_pam_check(yb_test_challenges == 1, "password read retried after an error");
	if (yb_pam_case_is("start_failure") || yb_pam_case_is("user_failure") ||
		yb_pam_case_is("rhost_failure") || yb_pam_case_is("conversation_failure") ||
		yb_pam_case_is("hostname_failure"))
		yb_pam_check(yb_test_auth_calls == 0 && yb_test_account_calls == 0,
					 "authentication continued after setup failed");
	if (yb_pam_case_is("auth_failure") || yb_pam_case_is("auth_eof") ||
		yb_pam_case_is("auth_error") || yb_pam_case_is("ignored_auth_error") ||
		yb_pam_case_is("unknown_user") ||
		yb_pam_case_is("directory_unavailable") || yb_pam_case_is("max_attempts"))
		yb_pam_check(yb_test_account_calls == 0, "account check ran after authentication failed");
	if (yb_pam_case_is("local_success"))
		yb_pam_check(yb_test_host_calls == 0, "local authentication resolved a remote host");
	if (!yb_pam_case_is("start_failure"))
		yb_pam_check(yb_test_end_status == expected_status, "wrong status passed to pam_end");
	yb_pam_check(interrupted == expect_interrupt, "PostgreSQL error was not rethrown");
	if (!interrupted)
		yb_pam_check(result == expected_result, "authentication result changed during cleanup");
	yb_pam_check(pam_passwd == NULL, "password pointer retained after authentication");
	yb_pam_check(pam_port_cludge == NULL && pam_passw_conv.appdata_ptr == NULL,
				 "conversation pointers retained after authentication");
	yb_pam_check((port.authn_id != NULL) == (expected_result == STATUS_OK),
				 "authenticated identity does not match the result");
	if (port.authn_id)
		pfree(unconstify(char *, port.authn_id));
	PG_RETURN_BOOL(true);
#else
	ereport(ERROR, (errmsg("PAM support is required for this test")));
	PG_RETURN_BOOL(false);
#endif
}

Datum
yb_test_pam_conversation(PG_FUNCTION_ARGS)
{
#ifdef USE_PAM
	struct pam_message messages[3] = {
		{PAM_TEXT_INFO, "test information"},
		{PAM_ERROR_MSG, "test diagnostic"},
		{PAM_PROMPT_ECHO_OFF, "Password:"}
	};
	const struct pam_message *pointers[3] = {&messages[0], &messages[1], &messages[2]};
	struct pam_response *responses = NULL;
	int			count = 3;
	volatile int result = PAM_CONV_ERR;
	int			expected = PAM_SUCCESS;
	const char *password = "dummy_token";
	Port		port = {0};
	HbaLine		hba = {0};
	MemoryContext oldcontext = CurrentMemoryContext;
	volatile bool interrupted = false;
	bool		expect_interrupt;
	bool		request_password;
	int			outstanding;

	yb_test_case = text_to_cstring(PG_GETARG_TEXT_PP(0));
	yb_test_allocation_count = 0;
	yb_test_challenges = 0;
	pam_no_password = false;
	expect_interrupt = yb_pam_case_is("malformed_password") ||
		yb_pam_case_is("empty_password") || yb_pam_case_is("interrupted_password");
	request_password = expect_interrupt || yb_pam_case_is("password_challenge") ||
		yb_pam_case_is("password_eof");
	if (request_password)
	{
		password = "";
		if (yb_pam_case_is("password_eof"))
			expected = PAM_CONV_ERR;
	}
	else if (yb_pam_case_is("zero_messages") || yb_pam_case_is("negative_messages") ||
		yb_pam_case_is("too_many_messages"))
	{
		count = yb_pam_case_is("zero_messages") ? 0 :
			yb_pam_case_is("negative_messages") ? -1 : PAM_MAX_NUM_MSG + 1;
		expected = PAM_CONV_ERR;
	}
	else if (yb_pam_case_is("echo_on") || yb_pam_case_is("unknown_style"))
	{
		/* Reject after a password was copied, exercising partial-response cleanup */
		messages[0].msg_style = PAM_PROMPT_ECHO_OFF;
		messages[1].msg_style = yb_pam_case_is("echo_on") ? PAM_PROMPT_ECHO_ON : -1;
		expected = PAM_CONV_ERR;
	}
	else if (yb_pam_case_is("allocation_failure") || yb_pam_case_is("response_failure"))
		expected = PAM_CONV_ERR;
	else
		yb_pam_check(yb_pam_case_is("mixed_messages") || yb_pam_case_is("fallback_password"),
					 "unknown conversation case");

	pam_passwd = password;
	pam_port_cludge = &port;
	PG_TRY();
	{
		if (expect_interrupt)
		{
			hba.conntype = ctLocal;
			hba.auth_method = uaPAM;
			hba.pamservice = "yb-unit-test";
			port.hba = &hba;
			yb_test_end_calls = 0;
			yb_test_handle_active = false;
			yb_test_module_active = false;
			result = CheckPAMAuth(&port, "alice@example.com", "");
		}
		else
			result = pam_passwd_conv_proc(count, pointers, &responses,
										 yb_pam_case_is("fallback_password") ? NULL : (void *) password);
	}
	PG_CATCH();
	{
		MemoryContextSwitchTo(oldcontext);
		if (!expect_interrupt)
			PG_RE_THROW();
		yb_pam_check(geterrcode() == (yb_pam_case_is("malformed_password") ?
									ERRCODE_PROTOCOL_VIOLATION :
									yb_pam_case_is("empty_password") ? ERRCODE_INVALID_PASSWORD :
									ERRCODE_QUERY_CANCELED), "wrong password error propagated");
		FlushErrorState();
		interrupted = true;
	}
	PG_END_TRY();
	pam_passwd = NULL;
	pam_port_cludge = NULL;
	if (!interrupted && result == PAM_SUCCESS)
	{
		yb_pam_check(responses != NULL, "missing conversation responses");
		for (int i = 0; i < count; i++)
		{
			yb_pam_check(responses[i].resp_retcode == PAM_SUCCESS &&
						 strcmp(responses[i].resp, i == 2 ? "dummy_token" : "") == 0,
						 "incorrect conversation response");
			yb_test_free(responses[i].resp);
		}
		yb_test_free(responses);
	}
	else
		yb_pam_check(responses == NULL, "partial responses exposed after failure");
	outstanding = yb_test_allocation_count;
	while (yb_test_allocation_count > 0)
		yb_test_free(yb_test_allocations[0]);
	yb_pam_check(outstanding == 0, "conversation memory leaked");
	yb_pam_check(interrupted == expect_interrupt, "password error was not rethrown");
	if (expect_interrupt)
		yb_pam_check(yb_test_end_calls == 1 && !yb_test_handle_active,
					 "PAM handle leaked after a password error");
	if (!interrupted)
		yb_pam_check(result == expected, "unexpected conversation status");
	yb_pam_check(yb_test_challenges == (request_password ? 1 : 0), "wrong password challenge count");
	yb_pam_check(pam_no_password == yb_pam_case_is("password_eof"), "wrong password EOF state");
	PG_RETURN_BOOL(true);
#else
	ereport(ERROR, (errmsg("PAM support is required for this test")));
	PG_RETURN_BOOL(false);
#endif
}
