/*-------------------------------------------------------------------------
 *
 * qx_container_backend.h
 *	  Container backend contract scaffold for QhapaqXian Engine
 *
 * This module defines the broker-facing contract for the Stage 24 container
 * backend. It is intentionally narrower than a real OCI runtime integration:
 * the backend validates request shape, builds launch payloads, and parses
 * broker responses, but it does not itself launch containers.
 *
 *-------------------------------------------------------------------------
 */
#ifndef QX_CONTAINER_BACKEND_H
#define QX_CONTAINER_BACKEND_H

#include "postgres.h"

#define QX_CONTAINER_BACKEND_PROTOCOL		"qx.container.backend.v1"
#define QX_CONTAINER_BACKEND_PROVIDER_KIND	"container"
#define QX_CONTAINER_BACKEND_RUNTIME_CLASS	"container"
#define QX_CONTAINER_BACKEND_ENDPOINT_PREFIX "container://"
#define QX_CONTAINER_BACKEND_RECEIPT_SCHEMA "qx.receipt.v1"
#define QX_CONTAINER_BACKEND_EXPECTED_ATTESTATION \
	"container_receipt_verified"

typedef struct QxContainerBackendRequest
{
	char	   *phase;
	char	   *tool_name;
	char	   *principal_name;
	char	   *principal_runtime;
	char	   *provider_name;
	char	   *provider_kind;
	char	   *provider_endpoint;
	char	   *sandbox_name;
	char	   *profile_name;
	char	   *environment_mode;
	char	   *workdir_name;
	char	   *command_line;
	char	   *image_ref;
	char	   *seccomp_mode;
	char	   *oci_profile;
	char	   *receipt_schema;
	char	   *receipt_alg;
	char	   *receipt_nonce;
	char	   *attestation_mode;
	char	   *detail;
	int32		timeout_ms;
	int32		memory_kb;
	int32		process_limit;
	int32		token_charge;
	int32		cost_charge;
	bool		require_attestation;
	bool		allow_network;
	bool		allow_privilege_escalation;
	bool		readonly_rootfs;
} QxContainerBackendRequest;

typedef struct QxContainerBackendResponse
{
	bool		accepted;
	bool		launched;
	bool		verified;
	int32		exit_code;
	int32		wall_time_ms;
	int32		token_charge;
	int32		cost_charge;
	char	   *backend_protocol;
	char	   *phase;
	char	   *tool_name;
	char	   *principal_name;
	char	   *principal_runtime;
	char	   *provider_name;
	char	   *provider_kind;
	char	   *provider_endpoint;
	char	   *container_id;
	char	   *sandbox_profile;
	char	   *receipt_schema;
	char	   *receipt_alg;
	char	   *receipt_nonce;
	char	   *receipt_signature;
	char	   *attestation_mode;
	char	   *detail;
} QxContainerBackendResponse;

extern void qx_container_backend_request_init(QxContainerBackendRequest *request);
extern void qx_container_backend_request_free(QxContainerBackendRequest *request);
extern void qx_container_backend_response_init(QxContainerBackendResponse *response);
extern void qx_container_backend_response_free(QxContainerBackendResponse *response);
extern bool qx_container_backend_is_supported_provider_kind(const char *provider_kind);
extern bool qx_container_backend_is_supported_runtime_class(const char *principal_runtime);
extern const char *qx_container_backend_expected_attestation_mode(void);
extern void qx_container_backend_validate_request(
	const QxContainerBackendRequest *request);
extern void qx_container_backend_validate_response(
	const QxContainerBackendRequest *request,
	const QxContainerBackendResponse *response);
extern char *qx_container_backend_build_launch_request(
	const QxContainerBackendRequest *request);
extern void qx_container_backend_parse_launch_response(
	const char *payload,
	QxContainerBackendResponse *response);

#endif							/* QX_CONTAINER_BACKEND_H */
