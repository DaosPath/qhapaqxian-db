/*
 * qhapaqxian_tool_runner.c
 *
 * Deterministic external tool runner for the QhapaqXian runtime bootstrap.
 * It reads a key/value request file and writes metering plus an operator-
 * readable detail line to a response file.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#define getcwd _getcwd
#else
#include <unistd.h>
#endif

typedef struct Request
{
	char	phase[32];
	char	tool[128];
	char	handler[256];
	char	sandbox[64];
	char	principal[128];
	char	provider[128];
	char	provider_kind[64];
	char	provider_endpoint[256];
	char	receipt_schema[64];
	char	receipt_nonce[256];
	char	profile[64];
	char	workdir[260];
	long	task_oid;
	long	goal_length;
	long	timeout_ms;
	long	process_limit;
	int		input_present;
	int		path_present;
	int		require_attestation;
} Request;

static void
parse_sandbox_environment(Request *request)
{
	const char *profile;
	const char *timeout_ms;
	const char *process_limit;
	const char *path;

	profile = getenv("QX_SANDBOX_PROFILE");
	timeout_ms = getenv("QX_SANDBOX_TIMEOUT_MS");
	process_limit = getenv("QX_SANDBOX_PROCESS_LIMIT");
	path = getenv("PATH");

	if (profile != NULL)
		snprintf(request->profile, sizeof(request->profile), "%s", profile);
	if (timeout_ms != NULL)
		request->timeout_ms = strtol(timeout_ms, NULL, 10);
	if (process_limit != NULL)
		request->process_limit = strtol(process_limit, NULL, 10);
	request->path_present = (path != NULL && path[0] != '\0');
	if (getcwd(request->workdir, sizeof(request->workdir)) == NULL)
		snprintf(request->workdir, sizeof(request->workdir), "%s", "<unknown>");
}

static const char *
path_basename(const char *path)
{
	const char *slash;
	const char *backslash;
	const char *base;

	if (path == NULL || path[0] == '\0')
		return "<unknown>";

	slash = strrchr(path, '/');
	backslash = strrchr(path, '\\');
	base = slash;
	if (backslash != NULL && (base == NULL || backslash > base))
		base = backslash;

	return base != NULL ? base + 1 : path;
}

static int
parse_request(const char *path, Request *request)
{
	FILE   *file;
	char	line[512];

	memset(request, 0, sizeof(*request));

	file = fopen(path, "r");
	if (file == NULL)
		return 0;

	while (fgets(line, sizeof(line), file) != NULL)
	{
		char   *eq = strchr(line, '=');
		char   *key;
		char   *value;

		if (eq == NULL)
			continue;
		*eq = '\0';
		key = line;
		value = eq + 1;
		value[strcspn(value, "\r\n")] = '\0';

		if (strcmp(key, "PHASE") == 0)
			snprintf(request->phase, sizeof(request->phase), "%s", value);
		else if (strcmp(key, "TASK_OID") == 0)
			request->task_oid = strtol(value, NULL, 10);
		else if (strcmp(key, "GOAL_LENGTH") == 0)
			request->goal_length = strtol(value, NULL, 10);
		else if (strcmp(key, "INPUT_PRESENT") == 0)
			request->input_present = (strcmp(value, "true") == 0);
		else if (strcmp(key, "TOOL") == 0)
			snprintf(request->tool, sizeof(request->tool), "%s", value);
		else if (strcmp(key, "HANDLER") == 0)
			snprintf(request->handler, sizeof(request->handler), "%s", value);
		else if (strcmp(key, "SANDBOX") == 0)
			snprintf(request->sandbox, sizeof(request->sandbox), "%s", value);
		else if (strcmp(key, "PRINCIPAL") == 0)
			snprintf(request->principal, sizeof(request->principal), "%s", value);
		else if (strcmp(key, "PROVIDER") == 0)
			snprintf(request->provider, sizeof(request->provider), "%s", value);
		else if (strcmp(key, "PROVIDER_KIND") == 0)
			snprintf(request->provider_kind, sizeof(request->provider_kind), "%s", value);
		else if (strcmp(key, "PROVIDER_ENDPOINT") == 0)
			snprintf(request->provider_endpoint, sizeof(request->provider_endpoint), "%s", value);
		else if (strcmp(key, "RECEIPT_SCHEMA") == 0)
			snprintf(request->receipt_schema, sizeof(request->receipt_schema), "%s", value);
		else if (strcmp(key, "RECEIPT_NONCE") == 0)
			snprintf(request->receipt_nonce, sizeof(request->receipt_nonce), "%s", value);
		else if (strcmp(key, "REQUIRE_ATTESTATION") == 0)
			request->require_attestation = (strcmp(value, "true") == 0);
	}

	fclose(file);
	parse_sandbox_environment(request);
	return request->tool[0] != '\0' && request->principal[0] != '\0';
}

static int
sandbox_bonus(const char *sandbox)
{
	if (strcmp(sandbox, "isolated") == 0)
		return 13;
	if (strcmp(sandbox, "restricted") == 0)
		return 7;
	return 2;
}

int
main(int argc, char **argv)
{
	const char *request_path = NULL;
	const char *response_path = NULL;
	Request		request;
	FILE	   *response;
	int			i;
	int			phase_bonus;
	int			tokens;
	int			cost;

	for (i = 1; i < argc; i++)
	{
		if (strcmp(argv[i], "--request-file") == 0 && i + 1 < argc)
			request_path = argv[++i];
		else if (strcmp(argv[i], "--response-file") == 0 && i + 1 < argc)
			response_path = argv[++i];
	}

	if (request_path == NULL || response_path == NULL)
	{
		fprintf(stderr, "request/response file arguments are required\n");
		return 1;
	}

	if (!parse_request(request_path, &request))
	{
		fprintf(stderr, "could not parse request file\n");
		return 1;
	}

	phase_bonus = strcmp(request.phase, "resume") == 0 ? 11 : 5;
	tokens = (int) request.goal_length +
		(int) strlen(request.tool) +
		(int) strlen(request.handler) +
		(int) strlen(request.principal) +
		(request.input_present ? 11 : 3) +
		sandbox_bonus(request.sandbox) +
		phase_bonus;
	cost = (tokens / 5) + (strcmp(request.phase, "resume") == 0 ? 4 : 2);
	if (cost < 1)
		cost = 1;

	response = fopen(response_path, "w");
	if (response == NULL)
	{
		fprintf(stderr, "could not open response file\n");
		return 1;
	}

	fprintf(response, "STATUS=ok\n");
	fprintf(response, "TOOL=%s\n", request.tool);
	fprintf(response, "PRINCIPAL=%s\n", request.principal);
	fprintf(response, "PROVIDER=%s\n", request.provider);
	fprintf(response, "SANDBOX=%s\n", request.sandbox);
	fprintf(response, "PROFILE=%s\n", request.profile[0] != '\0' ? request.profile : request.sandbox);
	fprintf(response, "ENV=%s\n", request.path_present ? "ambient" : "minimal");
	fprintf(response, "WORKDIR=%s\n", path_basename(request.workdir));
	fprintf(response, "TIMEOUT_MS=%ld\n", request.timeout_ms);
	fprintf(response, "PROCESS_LIMIT=%ld\n", request.process_limit);
	fprintf(response, "PATH_PRESENT=%s\n", request.path_present ? "true" : "false");
	fprintf(response, "RECEIPT_SCHEMA=%s\n", request.receipt_schema[0] != '\0' ? request.receipt_schema : "qx.receipt.v1");
	fprintf(response, "RECEIPT_NONCE=%s\n", request.receipt_nonce[0] != '\0' ? request.receipt_nonce : "missing");
	fprintf(response, "ATTESTATION=%s\n", request.require_attestation ? "loopback_verified" : "optional");
	fprintf(response, "TOKENS=%d\n", tokens);
	fprintf(response, "COST=%d\n", cost);
	fprintf(response, "DETAIL=tool %s via %s for %s phase on task %ld\n",
			request.tool,
			request.principal,
			request.phase[0] != '\0' ? request.phase : "submit",
			request.task_oid);

	fclose(response);
	return 0;
}
