/*-------------------------------------------------------------------------
 *
 * yb_test_pam_provider.c
 *    Deterministic PAM replacement for authentication protocol tests
 *
 * Copyright (c) YugabyteDB, Inc.
 *
 *-------------------------------------------------------------------------
 */

#include <arpa/inet.h>
#include <fcntl.h>
#include <security/pam_appl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The PAM ABI fixes the exported names and opaque handle tag */
struct pam_handle
{
	char	   *service;
	char	   *user;
	char	   *host;
	struct pam_conv conv;
	int			module_active;
};

int
pam_start(const char *service, const char *user, const struct pam_conv *conv,
		  pam_handle_t **handle)
{
	pam_handle_t *pam;

	*handle = NULL;
	if (strcmp(service, "yb-test-pam") != 0 && strcmp(service, "yb-test-pam-deny") != 0)
		return PAM_SERVICE_ERR;
	pam = calloc(1, sizeof(*pam));
	if (!pam)
		return PAM_BUF_ERR;
	pam->service = strdup(service);
	pam->user = strdup(user);
	pam->conv = *conv;
	if (!pam->service || !pam->user)
	{
		free(pam->service);
		free(pam->user);
		free(pam);
		return PAM_BUF_ERR;
	}
	*handle = pam;
	return PAM_SUCCESS;
}

int
pam_set_item(pam_handle_t *pam, int item, const void *value)
{
	char	  **target;
	char	   *copy;

	if (item == PAM_CONV)
	{
		pam->conv = *(const struct pam_conv *) value;
		return PAM_SUCCESS;
	}
	if (item != PAM_USER && item != PAM_RHOST)
		return PAM_BAD_ITEM;
	target = item == PAM_USER ? &pam->user : &pam->host;
	copy = strdup(value);
	if (!copy)
		return PAM_BUF_ERR;
	free(*target);
	*target = copy;
	return PAM_SUCCESS;
}

int
pam_authenticate(pam_handle_t *pam, int flags)
{
	struct pam_message messages[] = {
		{PAM_TEXT_INFO, "yb_test_pam information"},
		{PAM_ERROR_MSG, "yb_test_pam diagnostic"},
		{PAM_PROMPT_ECHO_OFF, "Password:"}
	};
	const struct pam_message *pointers[] = {&messages[0], &messages[1], &messages[2]};
	struct pam_response *response = NULL;
	int			count = strcmp(pam->user, "pam_messages") == 0 ? 3 : 1;
	char		expected[128];
	struct in_addr address;
	int			status;

	if (!pam->host || inet_pton(AF_INET, pam->host, &address) != 1)
		return PAM_SYSTEM_ERR;
	pam->module_active = 1;
	status = pam->conv.conv(count, pointers + 3 - count, &response, pam->conv.appdata_ptr);
	pam->module_active = 0;
	if (status != PAM_SUCCESS)
		return status;
	snprintf(expected, sizeof(expected), "%s_token", pam->user);
	status = response && response[count - 1].resp &&
		strcmp(response[count - 1].resp, expected) == 0 ?
		PAM_SUCCESS : PAM_AUTH_ERR;
	if (response)
	{
		for (int i = 0; i < count; i++)
			free(response[i].resp);
		free(response);
	}
	if (strcmp(pam->user, "pam_unknown") == 0)
		return PAM_USER_UNKNOWN;
	if (strcmp(pam->user, "pam_unavailable") == 0)
		return PAM_AUTHINFO_UNAVAIL;
	if (strcmp(pam->service, "yb-test-pam-deny") == 0)
		return PAM_AUTH_ERR;
	return status;
}

int
pam_acct_mgmt(pam_handle_t *pam, int flags)
{
	if (strcmp(pam->user, "pam_denied") == 0)
		return PAM_PERM_DENIED;
	if (strcmp(pam->user, "pam_expired") == 0)
		return PAM_ACCT_EXPIRED;
	if (strcmp(pam->user, "pam_password_expired") == 0)
		return PAM_NEW_AUTHTOK_REQD;
	return PAM_SUCCESS;
}

int
pam_end(pam_handle_t *pam, int status)
{
	const char *path = getenv("YB_TEST_PAM_LOG");

	/* Match Linux-PAM: cleanup is invalid while a module is active */
	if (pam->module_active)
		return PAM_SYSTEM_ERR;
	if (path)
	{
		int			fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0600);

		if (fd >= 0)
		{
			char		line[512];
			int			length = snprintf(line, sizeof(line), "%ld|%s|%s|%d\n",
										 (long) getpid(), pam->user,
										 pam->host ? pam->host : "", status);

			if (length > 0 && length < sizeof(line))
				(void) write(fd, line, length);
			close(fd);
		}
	}
	free(pam->service);
	free(pam->user);
	free(pam->host);
	free(pam);
	return PAM_SUCCESS;
}

const char *
pam_strerror(pam_handle_t *pam, int status)
{
	return "yb_test_pam rejection";
}
