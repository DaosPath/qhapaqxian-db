/*-------------------------------------------------------------------------
 *
 * qx_microvm_backend.h
 *	  MicroVM backend contract scaffold for QhapaqXian Engine
 *
 * This module defines the broker-facing contract for the Stage 25 microVM
 * backend. It stays intentionally narrower than a real hypervisor integration:
 * it validates request shape, builds launch payloads, and parses broker
 * responses, but it does not launch microVMs itself.
 *
 *-------------------------------------------------------------------------
 */
#ifndef QX_MICROVM_BACKEND_H
#define QX_MICROVM_BACKEND_H

#include "postgres.h"

#define QX_MICROVM_BACKEND_PROTOCOL		"qx.microvm.backend.v1"
#define QX_MICROVM_BACKEND_PROVIDER_KIND	"microvm"
#define QX_MICROVM_BACKEND_RUNTIME_CLASS	"microvm"
#define QX_MICROVM_BACKEND_ENDPOINT_PREFIX "microvm://"
#define QX_MICROVM_BACKEND_RECEIPT_SCHEMA "qx.receipt.v1"
#define QX_MICROVM_BACKEND_EXPECTED_ATTESTATION \
	"microvm_receipt_verified"

typedef struct QxMicrovmBackendRequest
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
	char	   *snapshot_ref;
	char	   *receipt_schema;
	char	   *receipt_alg;
	char	   *receipt_nonce;
	char	   *attestation_mode;
	char	   *detail;
	int32		timeout_ms;
	int32		memory_kb;
	int32		process_limit;
	int32		vcpu_count;
	int32		token_charge;
	int32		cost_charge;
	bool		require_attestation;
	bool		allow_network;
	bool		allow_privilege_escalation;
} QxMicrovmBackendRequest;

typedef struct QxMicrovmBackendResponse
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
	char	   *vm_id;
	char	   *snapshot_id;
	char	   *sandbox_profile;
	char	   *receipt_schema;
	char	   *receipt_alg;
	char	   *receipt_nonce;
	char	   *receipt_signature;
	char	   *attestation_mode;
	char	   *attestation_report;
	char	   *detail;
} QxMicrovmBackendResponse;

extern void qx_microvm_backend_request_init(QxMicrovmBackendRequest *request);
extern void qx_microvm_backend_request_free(QxMicrovmBackendRequest *request);
extern void qx_microvm_backend_response_init(QxMicrovmBackendResponse *response);
extern void qx_microvm_backend_response_free(QxMicrovmBackendResponse *response);
extern bool qx_microvm_backend_is_supported_provider_kind(const char *provider_kind);
extern bool qx_microvm_backend_is_supported_runtime_class(const char *principal_runtime);
extern const char *qx_microvm_backend_expected_attestation_mode(void);
extern void qx_microvm_backend_validate_request(
	const QxMicrovmBackendRequest *request);
extern void qx_microvm_backend_validate_response(
	const QxMicrovmBackendRequest *request,
	const QxMicrovmBackendResponse *response);
extern char *qx_microvm_backend_build_receipt_payload(
	const QxMicrovmBackendRequest *request,
	const char *phase);
extern char *qx_microvm_backend_build_attestation_mode(
	const QxMicrovmBackendRequest *request);
extern char *qx_microvm_backend_build_launch_request(
	const QxMicrovmBackendRequest *request);
extern void qx_microvm_backend_parse_launch_response(
	const char *payload,
	QxMicrovmBackendResponse *response);

#endif							/* QX_MICROVM_BACKEND_H */
