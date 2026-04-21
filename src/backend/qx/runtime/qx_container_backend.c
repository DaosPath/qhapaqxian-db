/*-------------------------------------------------------------------------
 *
 * qx_container_backend.c
 *	  Container backend contract scaffold for QhapaqXian Engine
 *
 * This file intentionally stops at the broker contract boundary. It is a
 * structural landing zone for future real container execution support:
 * request validation, launch payload assembly, and broker response parsing
 * are here; OCI/container runtime integration is deferred.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <errno.h>

#include "lib/stringinfo.h"
#include "qx_container_backend.h"
#include "utils/builtins.h"

static bool qx_container_backend_parse_bool(const char *value,
										   const char *fieldname);
static int32 qx_container_backend_parse_int32(const char *value,
											  const char *fieldname);
static void qx_container_backend_free_string(char **ptr);
static void qx_container_backend_append_field(StringInfoData *buf,
											  const char *key,
											  const char *value);
static void qx_container_backend_append_bool_field(StringInfoData *buf,
												   const char *key,
												   bool value);
static void qx_container_backend_append_int_field(StringInfoData *buf,
												  const char *key,
												  int32 value);

void
qx_container_backend_request_init(QxContainerBackendRequest *request)
{
	Assert(request != NULL);
	MemSet(request, 0, sizeof(QxContainerBackendRequest));
}

void
qx_container_backend_request_free(QxContainerBackendRequest *request)
{
	Assert(request != NULL);

	qx_container_backend_free_string(&request->phase);
	qx_container_backend_free_string(&request->tool_name);
	qx_container_backend_free_string(&request->principal_name);
	qx_container_backend_free_string(&request->principal_runtime);
	qx_container_backend_free_string(&request->provider_name);
	qx_container_backend_free_string(&request->provider_kind);
	qx_container_backend_free_string(&request->provider_endpoint);
	qx_container_backend_free_string(&request->sandbox_name);
	qx_container_backend_free_string(&request->profile_name);
	qx_container_backend_free_string(&request->environment_mode);
	qx_container_backend_free_string(&request->workdir_name);
	qx_container_backend_free_string(&request->command_line);
	qx_container_backend_free_string(&request->image_ref);
	qx_container_backend_free_string(&request->receipt_schema);
	qx_container_backend_free_string(&request->receipt_alg);
	qx_container_backend_free_string(&request->receipt_nonce);
	qx_container_backend_free_string(&request->attestation_mode);
	qx_container_backend_free_string(&request->detail);
	MemSet(request, 0, sizeof(QxContainerBackendRequest));
}

void
qx_container_backend_response_init(QxContainerBackendResponse *response)
{
	Assert(response != NULL);
	MemSet(response, 0, sizeof(QxContainerBackendResponse));
}

void
qx_container_backend_response_free(QxContainerBackendResponse *response)
{
	Assert(response != NULL);

	qx_container_backend_free_string(&response->backend_protocol);
	qx_container_backend_free_string(&response->phase);
	qx_container_backend_free_string(&response->tool_name);
	qx_container_backend_free_string(&response->principal_name);
	qx_container_backend_free_string(&response->principal_runtime);
	qx_container_backend_free_string(&response->provider_name);
	qx_container_backend_free_string(&response->provider_kind);
	qx_container_backend_free_string(&response->provider_endpoint);
	qx_container_backend_free_string(&response->container_id);
	qx_container_backend_free_string(&response->sandbox_profile);
	qx_container_backend_free_string(&response->receipt_schema);
	qx_container_backend_free_string(&response->receipt_alg);
	qx_container_backend_free_string(&response->receipt_nonce);
	qx_container_backend_free_string(&response->receipt_signature);
	qx_container_backend_free_string(&response->attestation_mode);
	qx_container_backend_free_string(&response->detail);
	MemSet(response, 0, sizeof(QxContainerBackendResponse));
}

bool
qx_container_backend_is_supported_provider_kind(const char *provider_kind)
{
	return (provider_kind != NULL &&
			pg_strcasecmp(provider_kind, QX_CONTAINER_BACKEND_PROVIDER_KIND) == 0);
}

bool
qx_container_backend_is_supported_runtime_class(const char *principal_runtime)
{
	return (principal_runtime != NULL &&
			pg_strcasecmp(principal_runtime, QX_CONTAINER_BACKEND_RUNTIME_CLASS) == 0);
}

const char *
qx_container_backend_expected_attestation_mode(void)
{
	return QX_CONTAINER_BACKEND_EXPECTED_ATTESTATION;
}

static void
qx_container_backend_require(bool condition, const char *message)
{
	if (!condition)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("%s", message)));
}

static void
qx_container_backend_free_string(char **ptr)
{
	if (ptr != NULL && *ptr != NULL)
	{
		pfree(*ptr);
		*ptr = NULL;
	}
}

void
qx_container_backend_validate_request(const QxContainerBackendRequest *request)
{
	qx_container_backend_require(request != NULL,
								 "container backend request must not be null");
	qx_container_backend_require(request->phase != NULL && request->phase[0] != '\0',
								 "container backend request requires a phase");
	qx_container_backend_require(request->tool_name != NULL && request->tool_name[0] != '\0',
								 "container backend request requires a tool name");
	qx_container_backend_require(request->principal_name != NULL && request->principal_name[0] != '\0',
								 "container backend request requires a principal name");
	qx_container_backend_require(qx_container_backend_is_supported_provider_kind(request->provider_kind),
								 "container backend only accepts provider kind container");
	qx_container_backend_require(qx_container_backend_is_supported_runtime_class(request->principal_runtime),
								 "container backend only accepts principal runtime class container");
	qx_container_backend_require(request->provider_endpoint != NULL &&
								 strstr(request->provider_endpoint,
										QX_CONTAINER_BACKEND_ENDPOINT_PREFIX) == request->provider_endpoint,
								 "container backend requires a container:// provider endpoint");
	qx_container_backend_require(request->sandbox_name != NULL &&
								 pg_strcasecmp(request->sandbox_name, "isolated") == 0,
								 "container backend requires isolated sandboxing");
	qx_container_backend_require(request->receipt_schema != NULL &&
								 pg_strcasecmp(request->receipt_schema,
											   QX_CONTAINER_BACKEND_RECEIPT_SCHEMA) == 0,
								 "container backend requires qx.receipt.v1 receipts");
	qx_container_backend_require(request->require_attestation ?
								 (request->receipt_alg != NULL &&
								  pg_strcasecmp(request->receipt_alg, "ed25519") == 0) : true,
								 "container backend requires ed25519 when attestation is enabled");
	qx_container_backend_require(request->attestation_mode == NULL ||
								 pg_strcasecmp(request->attestation_mode,
											   QX_CONTAINER_BACKEND_EXPECTED_ATTESTATION) == 0,
								 "container backend requires container_receipt_verified attestation");
	qx_container_backend_require(request->command_line != NULL &&
								 request->command_line[0] != '\0',
								 "container backend requires a command line");
}

void
qx_container_backend_validate_response(const QxContainerBackendRequest *request,
									   const QxContainerBackendResponse *response)
{
	qx_container_backend_require(request != NULL,
								 "container backend response validation requires a request");
	qx_container_backend_require(response != NULL,
								 "container backend response must not be null");
	qx_container_backend_require(response->accepted,
								 "container backend response was not accepted by the broker");
	qx_container_backend_require(response->launched,
								 "container backend response did not report a launch");
	qx_container_backend_require(response->verified,
								 "container backend response did not verify attestation");
	qx_container_backend_require(response->backend_protocol != NULL &&
								 pg_strcasecmp(response->backend_protocol,
											   QX_CONTAINER_BACKEND_PROTOCOL) == 0,
								 "container backend response protocol mismatch");
	qx_container_backend_require(response->principal_runtime != NULL &&
								 qx_container_backend_is_supported_runtime_class(response->principal_runtime),
								 "container backend response reported the wrong runtime class");
	qx_container_backend_require(response->provider_kind != NULL &&
								 qx_container_backend_is_supported_provider_kind(response->provider_kind),
								 "container backend response reported the wrong provider kind");
	qx_container_backend_require(response->provider_endpoint != NULL &&
								 strstr(response->provider_endpoint,
										QX_CONTAINER_BACKEND_ENDPOINT_PREFIX) == response->provider_endpoint,
								 "container backend response reported an invalid provider endpoint");
	qx_container_backend_require(response->receipt_schema != NULL &&
								 pg_strcasecmp(response->receipt_schema,
											   QX_CONTAINER_BACKEND_RECEIPT_SCHEMA) == 0,
								 "container backend response reported an invalid receipt schema");
	qx_container_backend_require(response->receipt_signature != NULL &&
								 response->receipt_signature[0] != '\0',
								 "container backend response is missing the receipt signature");
	qx_container_backend_require(response->attestation_mode != NULL &&
								 pg_strcasecmp(response->attestation_mode,
											   QX_CONTAINER_BACKEND_EXPECTED_ATTESTATION) == 0,
								 "container backend response reported the wrong attestation mode");
	qx_container_backend_require(response->container_id != NULL &&
								 response->container_id[0] != '\0',
								 "container backend response is missing a container identifier");
	qx_container_backend_require(response->exit_code >= 0,
								 "container backend response exit code must be non-negative");

	if (request->require_attestation)
		qx_container_backend_require(response->receipt_alg != NULL &&
									 pg_strcasecmp(response->receipt_alg, "ed25519") == 0,
									 "container backend response must preserve ed25519 receipts");
}

static void
qx_container_backend_append_field(StringInfoData *buf, const char *key,
								  const char *value)
{
	appendStringInfo(buf, "%s=%s\n", key, value != NULL ? value : "");
}

static void
qx_container_backend_append_bool_field(StringInfoData *buf, const char *key,
									   bool value)
{
	appendStringInfo(buf, "%s=%s\n", key, value ? "true" : "false");
}

static void
qx_container_backend_append_int_field(StringInfoData *buf, const char *key,
									  int32 value)
{
	appendStringInfo(buf, "%s=%d\n", key, value);
}

char *
qx_container_backend_build_launch_request(const QxContainerBackendRequest *request)
{
	StringInfoData buf;

	qx_container_backend_validate_request(request);

	initStringInfo(&buf);
	qx_container_backend_append_field(&buf, "protocol",
									  QX_CONTAINER_BACKEND_PROTOCOL);
	qx_container_backend_append_field(&buf, "backend",
									  QX_CONTAINER_BACKEND_PROVIDER_KIND);
	qx_container_backend_append_field(&buf, "phase", request->phase);
	qx_container_backend_append_field(&buf, "tool", request->tool_name);
	qx_container_backend_append_field(&buf, "principal", request->principal_name);
	qx_container_backend_append_field(&buf, "principal_runtime",
									  request->principal_runtime);
	qx_container_backend_append_field(&buf, "provider", request->provider_name);
	qx_container_backend_append_field(&buf, "provider_kind", request->provider_kind);
	qx_container_backend_append_field(&buf, "provider_endpoint",
									  request->provider_endpoint);
	qx_container_backend_append_field(&buf, "sandbox", request->sandbox_name);
	qx_container_backend_append_field(&buf, "profile", request->profile_name);
	qx_container_backend_append_field(&buf, "environment", request->environment_mode);
	qx_container_backend_append_field(&buf, "workdir", request->workdir_name);
	qx_container_backend_append_field(&buf, "command", request->command_line);
	qx_container_backend_append_field(&buf, "image_ref", request->image_ref);
	qx_container_backend_append_field(&buf, "receipt_schema",
									  request->receipt_schema);
	qx_container_backend_append_field(&buf, "receipt_alg", request->receipt_alg);
	qx_container_backend_append_field(&buf, "receipt_nonce", request->receipt_nonce);
	qx_container_backend_append_field(&buf, "attestation", request->attestation_mode);
	qx_container_backend_append_int_field(&buf, "timeout_ms", request->timeout_ms);
	qx_container_backend_append_int_field(&buf, "memory_kb", request->memory_kb);
	qx_container_backend_append_int_field(&buf, "process_limit", request->process_limit);
	qx_container_backend_append_int_field(&buf, "token_charge", request->token_charge);
	qx_container_backend_append_int_field(&buf, "cost_charge", request->cost_charge);
	qx_container_backend_append_bool_field(&buf, "require_attestation",
										   request->require_attestation);
	qx_container_backend_append_bool_field(&buf, "allow_network",
										   request->allow_network);
	qx_container_backend_append_bool_field(&buf,
										   "allow_privilege_escalation",
										   request->allow_privilege_escalation);
	qx_container_backend_append_field(&buf, "detail", request->detail);

	return buf.data;
}

static bool
qx_container_backend_parse_bool(const char *value, const char *fieldname)
{
	if (value == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("container backend response is missing %s", fieldname)));

	if (pg_strcasecmp(value, "true") == 0 || strcmp(value, "1") == 0 ||
		pg_strcasecmp(value, "yes") == 0)
		return true;

	if (pg_strcasecmp(value, "false") == 0 || strcmp(value, "0") == 0 ||
		pg_strcasecmp(value, "no") == 0)
		return false;

	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("invalid boolean value for %s: \"%s\"", fieldname, value)));
	return false;
}

static int32
qx_container_backend_parse_int32(const char *value, const char *fieldname)
{
	long		parsed;
	char	   *endptr;

	if (value == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("container backend response is missing %s", fieldname)));

	errno = 0;
	parsed = strtol(value, &endptr, 10);
	if (errno != 0 || endptr == value || *endptr != '\0' ||
		parsed < PG_INT32_MIN || parsed > PG_INT32_MAX)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("invalid integer value for %s: \"%s\"", fieldname, value)));

	return (int32) parsed;
}

void
qx_container_backend_parse_launch_response(const char *payload,
										   QxContainerBackendResponse *response)
{
	char	   *copy;
	char	   *cursor;

	Assert(response != NULL);
	qx_container_backend_response_init(response);
	qx_container_backend_require(payload != NULL && payload[0] != '\0',
								 "container backend response payload must not be empty");

	copy = pstrdup(payload);
	cursor = copy;

	while (cursor != NULL && *cursor != '\0')
	{
		char	   *line = cursor;
		char	   *next = strchr(cursor, '\n');
		char	   *eq;

		if (next != NULL)
			*next++ = '\0';

		if (line[0] != '\0' && line[0] != '#')
		{
			eq = strchr(line, '=');
			if (eq != NULL)
			{
				char	   *key = line;
				char	   *value = eq + 1;

				*eq = '\0';
				if (strcmp(key, "protocol") == 0)
					response->backend_protocol = pstrdup(value);
				else if (strcmp(key, "phase") == 0)
					response->phase = pstrdup(value);
				else if (strcmp(key, "tool") == 0)
					response->tool_name = pstrdup(value);
				else if (strcmp(key, "principal") == 0)
					response->principal_name = pstrdup(value);
				else if (strcmp(key, "principal_runtime") == 0)
					response->principal_runtime = pstrdup(value);
				else if (strcmp(key, "provider") == 0)
					response->provider_name = pstrdup(value);
				else if (strcmp(key, "provider_kind") == 0)
					response->provider_kind = pstrdup(value);
				else if (strcmp(key, "provider_endpoint") == 0)
					response->provider_endpoint = pstrdup(value);
				else if (strcmp(key, "container_id") == 0)
					response->container_id = pstrdup(value);
				else if (strcmp(key, "sandbox") == 0)
					response->sandbox_profile = pstrdup(value);
				else if (strcmp(key, "receipt_schema") == 0)
					response->receipt_schema = pstrdup(value);
				else if (strcmp(key, "receipt_alg") == 0)
					response->receipt_alg = pstrdup(value);
				else if (strcmp(key, "receipt_nonce") == 0)
					response->receipt_nonce = pstrdup(value);
				else if (strcmp(key, "receipt_signature") == 0)
					response->receipt_signature = pstrdup(value);
				else if (strcmp(key, "attestation") == 0)
					response->attestation_mode = pstrdup(value);
				else if (strcmp(key, "detail") == 0)
					response->detail = pstrdup(value);
				else if (strcmp(key, "accepted") == 0)
					response->accepted = qx_container_backend_parse_bool(value, key);
				else if (strcmp(key, "launched") == 0)
					response->launched = qx_container_backend_parse_bool(value, key);
				else if (strcmp(key, "verified") == 0)
					response->verified = qx_container_backend_parse_bool(value, key);
				else if (strcmp(key, "exit_code") == 0)
					response->exit_code = qx_container_backend_parse_int32(value, key);
				else if (strcmp(key, "wall_ms") == 0)
					response->wall_time_ms = qx_container_backend_parse_int32(value, key);
				else if (strcmp(key, "token_charge") == 0)
					response->token_charge = qx_container_backend_parse_int32(value, key);
				else if (strcmp(key, "cost_charge") == 0)
					response->cost_charge = qx_container_backend_parse_int32(value, key);
			}
		}

		cursor = next;
	}

	pfree(copy);
}
