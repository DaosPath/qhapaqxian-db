/*-------------------------------------------------------------------------
 *
 * qx_runtime.c
 *	  Stage 7 embedded runtime entry points for QhapaqXian Engine
 *
 * Copyright (c) 1996-2024, PostgreSQL Global Development Group
 *
 * src/backend/qx/runtime/qx_runtime.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/table.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/objectaccess.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_qx_agent.h"
#include "catalog/pg_qx_attempt.h"
#include "catalog/pg_qx_checkpoint.h"
#include "catalog/pg_qx_event.h"
#include "catalog/pg_qx_identity.h"
#include "catalog/pg_qx_namespace.h"
#include "catalog/pg_qx_provider.h"
#include "catalog/pg_qx_session.h"
#include "catalog/pg_qx_step.h"
#include "catalog/pg_qx_task.h"
#include "catalog/pg_qx_trace.h"
#include "common/hmac.h"
#include "common/openssl.h"
#include "common/sha2.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "nodes/readfuncs.h"
#include "nodes/value.h"
#include "port.h"
#include "qx/qx_runtime.h"
#include "qx/qx_semantic_log.h"
#include "storage/fd.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/pg_lsn.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"

#ifdef USE_OPENSSL
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#endif

#include <sys/stat.h>

#ifndef WIN32
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#else
#include <io.h>
#include <windows.h>
#endif

typedef struct QxExternalToolResult
{
	int32		token_charge;
	int32		cost_charge;
	int32		timeout_ms;
	int32		process_limit;
	bool		path_present;
	char	   *detail;
	char	   *tool_name;
	char	   *principal_name;
	char	   *principal_runtime;
	char	   *provider_name;
	char	   *provider_kind;
	char	   *sandbox_name;
	char	   *profile_name;
	char	   *environment_mode;
	char	   *workdir_name;
	char	   *receipt_schema;
	char	   *receipt_alg;
	char	   *receipt_nonce;
	char	   *receipt_signature;
	char	   *attestation_mode;
} QxExternalToolResult;

typedef struct QxSandboxProfile
{
	const char *name;
	int32		timeout_ms;
	int32		memory_kb;
	int32		file_kb;
	int32		max_open_files;
	int32		process_limit;
} QxSandboxProfile;

typedef struct QxSandboxObservation
{
	int32		wall_time_ms;
	const char *launch_mode;
	bool		restricted_identity;
} QxSandboxObservation;

static uint32 qx_runtime_temp_seq = 0;
#define QX_RECEIPT_SIG_HEX_LEN	((PG_SHA256_DIGEST_LENGTH * 2) + 1)
#define QX_ED25519_SIG_BYTES	64
#define QX_ED25519_SIG_HEX_LEN	((QX_ED25519_SIG_BYTES * 2) + 1)

static char *qx_text_attr_from_syscache(HeapTuple tup, AttrNumber attnum,
										int cacheid);
static char *qx_contract_value(const char *contract, const char *key);
static char *qx_provider_receipt_key(Oid provideroid);
static const char *qx_default_runtime_for_provider_kind(const char *provider_kind);
static char *qx_expected_attestation_mode(const char *provider_kind,
										  bool require_attestation);
static char *qx_receipt_hmac_signature_hex(const char *receipt_key,
										   const char *payload);
#ifdef USE_OPENSSL
static char *qx_openssl_error_string(void);
static bool qx_verify_ed25519_receipt_signature(const char *public_key_pem,
												 const char *payload,
												 const char *signature_hex);
#endif
static char *qx_receipt_payload(const char *phase,
								Oid taskoid,
								const char *tool_name,
								const char *principal_name,
								const char *principal_runtime,
								const char *provider_name,
								const char *provider_kind,
								const char *provider_endpoint,
								const char *sandbox_name,
								const char *profile_name,
								const char *environment_mode,
								const char *workdir_name,
								int32 timeout_ms,
								int32 process_limit,
								bool path_present,
								const char *receipt_schema,
								const char *receipt_alg,
								const char *receipt_nonce,
								const char *attestation_mode,
								int32 token_charge,
								int32 cost_charge,
								const char *detail);
static int	qx_sandbox_rank(const char *sandbox_name);
static const char *qx_effective_sandbox_name(const char *tool_sandbox,
											 const char *principal_sandbox);
static const QxSandboxProfile *qx_lookup_sandbox_profile(const char *sandbox_name);
static void qx_runtime_temp_dir(char *path, size_t pathlen);
static void qx_runtime_temp_path(char *path, size_t pathlen,
								 const char *suffix);
static void qx_write_text_file(const char *path, const char *contents);
static void qx_resolve_principal_program_path(const char *program_name,
											  char *resolved,
											  size_t resolved_len);
static void qx_resolve_receipt_signer_path(const char *signer_name,
										   char *resolved,
										   size_t resolved_len);
static void qx_launch_principal_program(const char *program_path,
										const QxSandboxProfile *profile,
										const char *runtime_dir,
										const char *request_path,
										const char *response_path,
										QxSandboxObservation *observation);
static void qx_read_external_result(const char *path,
									QxExternalToolResult *result);
static void qx_validate_external_result(const QxExternalToolResult *result,
										const QxSandboxProfile *profile,
										const char *phase,
										Oid taskoid,
										const char *expected_tool,
										const char *expected_principal,
										const char *expected_principal_runtime,
										const char *expected_provider,
										const char *expected_provider_kind,
										const char *expected_provider_endpoint,
										const char *expected_receipt_schema,
										const char *expected_receipt_alg,
										const char *expected_receipt_nonce,
										const char *receipt_key,
										bool require_attestation);
static void qx_free_external_result(QxExternalToolResult *result);
static void qx_execute_tool_contract(const char *contract,
									 const char *phase,
									 Oid taskoid,
									 const char *goal,
									 bool input_present,
									 QxExternalToolResult *result);
static List *qx_fetch_task_authorized_contracts(HeapTuple tasktup);
static char *qx_fetch_task_goal(HeapTuple tasktup);
static bool qx_task_input_present(HeapTuple tasktup);

static void
qx_set_text_datum(Datum *values, bool *nulls, AttrNumber attnum,
				  const char *value)
{
	if (value == NULL)
	{
		nulls[attnum - 1] = true;
		return;
	}

	values[attnum - 1] = CStringGetTextDatum(value);
}

static void
qx_set_nodetree_datum(Datum *values, bool *nulls, AttrNumber attnum,
					  const void *node)
{
	char	   *serialized;

	if (node == NULL)
	{
		nulls[attnum - 1] = true;
		return;
	}

	serialized = nodeToString(node);
	values[attnum - 1] = CStringGetTextDatum(serialized);
	pfree(serialized);
}

static char *
qx_text_attr_from_syscache(HeapTuple tup, AttrNumber attnum, int cacheid)
{
	bool		isnull;
	Datum		datum;

	datum = SysCacheGetAttr(cacheid, tup, attnum, &isnull);
	if (isnull)
		return NULL;

	return TextDatumGetCString(datum);
}

static char *
qx_contract_value(const char *contract, const char *key)
{
	char	   *pattern;
	char	   *start;
	char	   *end;
	char	   *value;

	if (contract == NULL || key == NULL)
		return NULL;

	pattern = psprintf("%s=", key);
	start = strstr(contract, pattern);
	pfree(pattern);
	if (start == NULL)
		return NULL;

	start += strlen(key) + 1;
	end = strchr(start, ';');
	if (end == NULL)
		end = unconstify(char *, contract) + strlen(contract);

	value = pnstrdup(start, end - start);
	return value;
}

static char *
qx_provider_receipt_key(Oid provideroid)
{
	HeapTuple	providertup;
	Form_pg_qx_provider providerform;
	char	   *receipt_key;

	providertup = SearchSysCache1(QXPROVIDEROID, ObjectIdGetDatum(provideroid));
	if (!HeapTupleIsValid(providertup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("provider %u no longer exists for runtime receipt verification",
						provideroid)));

	providerform = (Form_pg_qx_provider) GETSTRUCT(providertup);
	if (!providerform->qxproviderenabled)
	{
		ReleaseSysCache(providertup);
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("provider %u is disabled for runtime receipt verification",
						provideroid)));
	}

	receipt_key = qx_text_attr_from_syscache(providertup,
											 Anum_pg_qx_provider_qxproviderreceiptkey,
											 QXPROVIDEROID);
	ReleaseSysCache(providertup);

	if (receipt_key == NULL || receipt_key[0] == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("provider %u has no receipt key configured", provideroid)));

	return receipt_key;
}

static const char *
qx_default_runtime_for_provider_kind(const char *provider_kind)
{
	if (provider_kind != NULL && strcmp(provider_kind, "container") == 0)
		return "container";
	if (provider_kind != NULL && strcmp(provider_kind, "microvm") == 0)
		return "microvm";

	return "host";
}

static char *
qx_expected_attestation_mode(const char *provider_kind, bool require_attestation)
{
	if (!require_attestation)
		return pstrdup("optional");

	if (provider_kind != NULL && strcmp(provider_kind, "microvm") == 0)
		return pstrdup("microvm_receipt_verified");

	if (provider_kind != NULL && strcmp(provider_kind, "container") == 0)
		return pstrdup("container_receipt_verified");

	if (provider_kind != NULL && strcmp(provider_kind, "remote") == 0)
		return pstrdup("remote_broker_verified");

	return pstrdup("loopback_verified");
}

static char *
qx_receipt_hmac_signature_hex(const char *receipt_key, const char *payload)
{
	pg_hmac_ctx *ctx;
	uint8		digest[PG_SHA256_DIGEST_LENGTH];
	char	   *hex;
	int			i;

	ctx = pg_hmac_create(PG_SHA256);
	if (ctx == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("could not allocate receipt verifier context")));

	if (pg_hmac_init(ctx, (const uint8 *) receipt_key, strlen(receipt_key)) < 0 ||
		pg_hmac_update(ctx, (const uint8 *) payload, strlen(payload)) < 0 ||
		pg_hmac_final(ctx, digest, sizeof(digest)) < 0)
	{
		const char *reason = pg_hmac_error(ctx);

		pg_hmac_free(ctx);
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("could not compute provider receipt signature"),
				 errdetail("%s", reason)));
	}

	pg_hmac_free(ctx);
	hex = palloc(QX_RECEIPT_SIG_HEX_LEN);
	for (i = 0; i < PG_SHA256_DIGEST_LENGTH; i++)
		snprintf(hex + (i * 2), 3, "%02x", digest[i]);
	hex[QX_RECEIPT_SIG_HEX_LEN - 1] = '\0';
	explicit_bzero(digest, sizeof(digest));

	return hex;
}

#ifdef USE_OPENSSL
static char *
qx_openssl_error_string(void)
{
	unsigned long errcode;
	char		buffer[256];

	errcode = ERR_get_error();
	if (errcode == 0)
		return pstrdup("no OpenSSL error reported");

	ERR_error_string_n(errcode, buffer, sizeof(buffer));
	return pstrdup(buffer);
}

static int
qx_hex_value(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

static bool
qx_hex_decode_signature(const char *hex, uint8 *dest, size_t expected_len)
{
	size_t		hexlen;
	size_t		i;

	if (hex == NULL)
		return false;

	hexlen = strlen(hex);
	if (hexlen != expected_len * 2)
		return false;

	for (i = 0; i < expected_len; i++)
	{
		int			hi = qx_hex_value(hex[i * 2]);
		int			lo = qx_hex_value(hex[(i * 2) + 1]);

		if (hi < 0 || lo < 0)
			return false;
		dest[i] = (uint8) ((hi << 4) | lo);
	}

	return true;
}

static bool
qx_verify_ed25519_receipt_signature(const char *public_key_pem,
									  const char *payload,
									  const char *signature_hex)
{
	BIO		   *bio = NULL;
	EVP_PKEY   *pkey = NULL;
	EVP_MD_CTX *mdctx = NULL;
	uint8		signature[QX_ED25519_SIG_BYTES];
	bool		verified = false;
	int			rc;

	if (public_key_pem == NULL || public_key_pem[0] == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("provider has no public key configured for ed25519 receipt verification")));

	if (!qx_hex_decode_signature(signature_hex, signature, sizeof(signature)))
		return false;

	bio = BIO_new_mem_buf(public_key_pem, -1);
	if (bio == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("could not allocate receipt public-key buffer")));

	pkey = PEM_read_bio_PUBKEY(bio, NULL, NULL, NULL);
	if (pkey == NULL)
	{
		char	   *detail = qx_openssl_error_string();

		BIO_free(bio);
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("could not load ed25519 receipt public key"),
				 errdetail("%s", detail)));
	}

	mdctx = EVP_MD_CTX_new();
	if (mdctx == NULL)
	{
		BIO_free(bio);
		EVP_PKEY_free(pkey);
		ereport(ERROR,
				(errcode(ERRCODE_OUT_OF_MEMORY),
				 errmsg("could not allocate ed25519 verifier context")));
	}

	rc = EVP_DigestVerifyInit(mdctx, NULL, NULL, NULL, pkey);
	if (rc != 1)
	{
		char	   *detail = qx_openssl_error_string();

		EVP_MD_CTX_free(mdctx);
		BIO_free(bio);
		EVP_PKEY_free(pkey);
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("could not initialize ed25519 receipt verifier"),
				 errdetail("%s", detail)));
	}

	rc = EVP_DigestVerify(mdctx,
						  signature, sizeof(signature),
						  (const unsigned char *) payload, strlen(payload));
	verified = (rc == 1);

	EVP_MD_CTX_free(mdctx);
	BIO_free(bio);
	EVP_PKEY_free(pkey);
	explicit_bzero(signature, sizeof(signature));

	return verified;
}
#endif

static char *
qx_receipt_payload(const char *phase,
				   Oid taskoid,
				   const char *tool_name,
				   const char *principal_name,
				   const char *principal_runtime,
				   const char *provider_name,
				   const char *provider_kind,
				   const char *provider_endpoint,
				   const char *sandbox_name,
				   const char *profile_name,
				   const char *environment_mode,
				   const char *workdir_name,
				   int32 timeout_ms,
				   int32 process_limit,
				   bool path_present,
				   const char *receipt_schema,
				   const char *receipt_alg,
				   const char *receipt_nonce,
				   const char *attestation_mode,
				   int32 token_charge,
				   int32 cost_charge,
				   const char *detail)
{
	return psprintf("phase=%s;task=%u;tool=%s;principal=%s;principal_runtime=%s;provider=%s;provider_kind=%s;provider_endpoint=%s;sandbox=%s;profile=%s;env=%s;workdir=%s;timeout_ms=%d;process_limit=%d;path_present=%s;receipt_schema=%s;receipt_alg=%s;receipt_nonce=%s;attestation=%s;tokens=%d;cost=%d;detail=%s",
					phase != NULL ? phase : "submit",
					taskoid,
					tool_name != NULL ? tool_name : "<unknown>",
					principal_name != NULL ? principal_name : "<unknown>",
					principal_runtime != NULL ? principal_runtime : "host",
					provider_name != NULL ? provider_name : "<unknown>",
					provider_kind != NULL ? provider_kind : "loopback",
					provider_endpoint != NULL ? provider_endpoint : "local://qhapaqxian-tool-runner",
					sandbox_name != NULL ? sandbox_name : "builtin",
					profile_name != NULL ? profile_name : "builtin",
					environment_mode != NULL ? environment_mode : "minimal",
					workdir_name != NULL ? workdir_name : "pg_qx_runtime",
					timeout_ms,
					process_limit,
					path_present ? "true" : "false",
					receipt_schema != NULL ? receipt_schema : "qx.receipt.v1",
					receipt_alg != NULL ? receipt_alg : "hmac-sha256",
					receipt_nonce != NULL ? receipt_nonce : "<unknown>",
					attestation_mode != NULL ? attestation_mode : "optional",
					token_charge,
					cost_charge,
					detail != NULL ? detail : "<none>");
}

static int
qx_sandbox_rank(const char *sandbox_name)
{
	if (sandbox_name == NULL || strcmp(sandbox_name, "builtin") == 0)
		return 0;
	if (strcmp(sandbox_name, "restricted") == 0)
		return 1;
	if (strcmp(sandbox_name, "isolated") == 0)
		return 2;

	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("unsupported sandbox \"%s\"", sandbox_name)));
	return -1;
}

static const char *
qx_effective_sandbox_name(const char *tool_sandbox,
						  const char *principal_sandbox)
{
	if (qx_sandbox_rank(principal_sandbox) >= qx_sandbox_rank(tool_sandbox))
		return principal_sandbox != NULL ? principal_sandbox : "builtin";

	return tool_sandbox != NULL ? tool_sandbox : "builtin";
}

static const QxSandboxProfile *
qx_lookup_sandbox_profile(const char *sandbox_name)
{
	static const QxSandboxProfile builtin_profile = {
		"builtin", 3000, 262144, 1024, 64, 2
	};
	static const QxSandboxProfile restricted_profile = {
		"restricted", 2000, 131072, 256, 32, 1
	};
	static const QxSandboxProfile isolated_profile = {
		"isolated", 1200, 65536, 64, 16, 1
	};

	if (sandbox_name == NULL || strcmp(sandbox_name, "builtin") == 0)
		return &builtin_profile;
	if (strcmp(sandbox_name, "restricted") == 0)
		return &restricted_profile;
	if (strcmp(sandbox_name, "isolated") == 0)
		return &isolated_profile;

	ereport(ERROR,
			(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
			 errmsg("unsupported sandbox profile \"%s\"", sandbox_name)));
	return NULL;
}

static void
qx_runtime_temp_dir(char *path, size_t pathlen)
{
	char		dir[MAXPGPATH];

	join_path_components(dir, DataDir, "pg_qx_runtime");
	if (MakePGDirectory(dir) < 0 && errno != EEXIST)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create QhapaqXian runtime directory \"%s\": %m",
						dir)));

	strlcpy(path, dir, pathlen);
}

static void
qx_runtime_temp_path(char *path, size_t pathlen, const char *suffix)
{
	char		dir[MAXPGPATH];
	char		filename[MAXPGPATH];

	qx_runtime_temp_dir(dir, sizeof(dir));
	qx_runtime_temp_seq++;
	snprintf(filename, sizeof(filename), "qx_tool_%d_%lld_%u.%s",
			 MyProcPid, (long long) GetCurrentTimestamp(),
			 qx_runtime_temp_seq, suffix);
	join_path_components(path, dir, filename);
}

static void
qx_write_text_file(const char *path, const char *contents)
{
	FILE	   *file;

	file = AllocateFile(path, "w");
	if (file == NULL)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not open runtime file \"%s\": %m", path)));

	if (fputs(contents, file) < 0)
	{
		FreeFile(file);
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not write runtime file \"%s\": %m", path)));
	}

	FreeFile(file);
}

static void
qx_resolve_principal_program_path(const char *program_name,
								  char *resolved,
								  size_t resolved_len)
{
	char		bindir[MAXPGPATH];
	char		candidate[MAXPGPATH];
	char		program_with_ext[MAXPGPATH];

	if (program_name == NULL || program_name[0] == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("principal program is not configured")));

	strlcpy(bindir, my_exec_path, sizeof(bindir));
	get_parent_directory(bindir);
	canonicalize_path(bindir);

	if (is_absolute_path(program_name))
		strlcpy(candidate, program_name, sizeof(candidate));
	else
	{
		strlcpy(program_with_ext, program_name, sizeof(program_with_ext));
#if defined(WIN32) && !defined(__CYGWIN__)
		if (strchr(program_with_ext, '.') == NULL)
			strlcat(program_with_ext, EXE, sizeof(program_with_ext));
#endif
		join_path_components(candidate, bindir, program_with_ext);
	}

	canonicalize_path(candidate);
	if (!path_is_prefix_of_path(bindir, candidate))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("principal program \"%s\" resolves outside the server bindir",
						program_name),
				 errdetail("Only executables shipped with the QhapaqXian installation are allowed for external tool principals.")));

	if (validate_exec(candidate) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("principal program \"%s\" is not executable", candidate)));

	strlcpy(resolved, candidate, resolved_len);
}

static void
qx_resolve_receipt_signer_path(const char *signer_name,
							   char *resolved,
							   size_t resolved_len)
{
	char		bindir[MAXPGPATH];
	char		candidate[MAXPGPATH];
	struct stat st;

	if (signer_name == NULL || signer_name[0] == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("principal receipt signer is not configured")));

	strlcpy(bindir, my_exec_path, sizeof(bindir));
	get_parent_directory(bindir);
	canonicalize_path(bindir);

	if (is_absolute_path(signer_name))
		strlcpy(candidate, signer_name, sizeof(candidate));
	else
		join_path_components(candidate, bindir, signer_name);

	canonicalize_path(candidate);
	if (!path_is_prefix_of_path(bindir, candidate))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("receipt signer \"%s\" resolves outside the server bindir",
						signer_name),
				 errdetail("Only signer files shipped with the QhapaqXian installation are allowed for asymmetric receipts.")));

	if (stat(candidate, &st) != 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("receipt signer \"%s\" is not readable", candidate)));

#ifndef WIN32
	if (S_ISDIR(st.st_mode))
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("receipt signer \"%s\" is not a file", candidate)));
#endif

	strlcpy(resolved, candidate, resolved_len);
}

static void
qx_launch_principal_program(const char *program_path,
							const QxSandboxProfile *profile,
							const char *runtime_dir,
							const char *request_path,
							const char *response_path,
							QxSandboxObservation *observation)
{
#ifndef WIN32
	pid_t		pid;
	int			status;
	bool		timed_out = false;
	TimestampTz	started_at;
	char	   *argv[6];
	char	   *envp[9];
	int			env_index = 0;

	argv[0] = unconstify(char *, program_path);
	argv[1] = "--request-file";
	argv[2] = unconstify(char *, request_path);
	argv[3] = "--response-file";
	argv[4] = unconstify(char *, response_path);
	argv[5] = NULL;
	observation->launch_mode = "profiled_process";
	observation->restricted_identity = false;
	observation->wall_time_ms = 0;

	envp[env_index++] = psprintf("QX_SANDBOX_PROFILE=%s", profile->name);
	envp[env_index++] = psprintf("QX_SANDBOX_TIMEOUT_MS=%d", profile->timeout_ms);
	envp[env_index++] = psprintf("QX_SANDBOX_MEMORY_KB=%d", profile->memory_kb);
	envp[env_index++] = psprintf("QX_SANDBOX_PROCESS_LIMIT=%d", profile->process_limit);
	envp[env_index++] = psprintf("QX_SANDBOX_MAX_OPEN_FILES=%d", profile->max_open_files);
	envp[env_index++] = psprintf("QX_RUNTIME_ROOT=%s", runtime_dir);
	envp[env_index++] = psprintf("TMPDIR=%s", runtime_dir);
	envp[env_index++] = pstrdup("LANG=C");
	envp[env_index] = NULL;

	pid = fork();
	if (pid < 0)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not fork principal program \"%s\": %m",
						program_path)));

	if (pid == 0)
	{
		long		maxfd;
		int			fd;

		if (chdir(runtime_dir) != 0)
			_exit(126);

#ifdef RLIMIT_NOFILE
		{
			struct rlimit limit;

			limit.rlim_cur = profile->max_open_files;
			limit.rlim_max = profile->max_open_files;
			(void) setrlimit(RLIMIT_NOFILE, &limit);
		}
#endif
#ifdef RLIMIT_FSIZE
		{
			struct rlimit limit;

			limit.rlim_cur = (rlim_t) profile->file_kb * 1024;
			limit.rlim_max = (rlim_t) profile->file_kb * 1024;
			(void) setrlimit(RLIMIT_FSIZE, &limit);
		}
#endif
#ifdef RLIMIT_AS
		{
			struct rlimit limit;

			limit.rlim_cur = (rlim_t) profile->memory_kb * 1024;
			limit.rlim_max = (rlim_t) profile->memory_kb * 1024;
			(void) setrlimit(RLIMIT_AS, &limit);
		}
#endif
#ifdef RLIMIT_CPU
		{
			struct rlimit limit;
			int32		cpu_seconds;

			cpu_seconds = (profile->timeout_ms + 999) / 1000;
			if (cpu_seconds <= 0)
				cpu_seconds = 1;
			limit.rlim_cur = cpu_seconds;
			limit.rlim_max = cpu_seconds;
			(void) setrlimit(RLIMIT_CPU, &limit);
		}
#endif

		maxfd = sysconf(_SC_OPEN_MAX);
		if (maxfd < 0 || maxfd > 1024)
			maxfd = 1024;
		for (fd = 3; fd < maxfd; fd++)
			close(fd);

		execve(program_path, argv, envp);
		_exit(127);
	}

	started_at = GetCurrentTimestamp();
	for (;;)
	{
		pid_t		wait_result;

		wait_result = waitpid(pid, &status, WNOHANG);
		if (wait_result == pid)
			break;
		if (wait_result < 0)
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not wait for principal program \"%s\": %m",
							program_path)));
		if (TimestampDifferenceExceeds(started_at, GetCurrentTimestamp(),
									   profile->timeout_ms))
		{
			(void) kill(pid, SIGKILL);
			(void) waitpid(pid, &status, 0);
			timed_out = true;
			break;
		}

		pg_usleep(10000L);
	}

	while (env_index > 0)
		pfree(envp[--env_index]);
	observation->wall_time_ms =
		(int32) TimestampDifferenceMilliseconds(started_at, GetCurrentTimestamp());

	if (timed_out)
		ereport(ERROR,
				(errcode(ERRCODE_QUERY_CANCELED),
				 errmsg("principal program \"%s\" exceeded sandbox timeout",
						program_path),
				 errdetail("Sandbox profile \"%s\" allows at most %d ms of wall time.",
						   profile->name, profile->timeout_ms)));

	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("principal program \"%s\" failed", program_path),
				 errdetail("Exit status was %d.", WIFEXITED(status) ? WEXITSTATUS(status) : -1)));
#else
	STARTUPINFOA si;
	PROCESS_INFORMATION pi;
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_limits;
	HANDLE		job;
	HANDLE		base_token = NULL;
	HANDLE		launch_token = NULL;
	StringInfoData environment;
	char		cmdline[(MAXPGPATH * 3) + 128];
	DWORD		exit_code = 0;
	DWORD		wait_status;
	const char *system_root = getenv("SystemRoot");
	char	   *timeout_value;
	char	   *memory_value;
	char	   *process_limit_value;
	char	   *nofile_value;
	bool		need_restricted_identity;
	TimestampTz	started_at;

	ZeroMemory(&si, sizeof(si));
	ZeroMemory(&pi, sizeof(pi));
	ZeroMemory(&job_limits, sizeof(job_limits));
	si.cb = sizeof(si);
	observation->launch_mode = "profiled_process";
	observation->restricted_identity = false;
	observation->wall_time_ms = 0;
	need_restricted_identity = (strcmp(profile->name, "builtin") != 0);

	job = CreateJobObjectA(NULL, NULL);
	if (job == NULL)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not create sandbox job object for principal \"%s\": %m (error code %lu)",
						program_path, GetLastError())));

	job_limits.BasicLimitInformation.LimitFlags =
		JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
		JOB_OBJECT_LIMIT_ACTIVE_PROCESS |
		JOB_OBJECT_LIMIT_PROCESS_MEMORY;
	job_limits.BasicLimitInformation.ActiveProcessLimit = profile->process_limit;
	job_limits.ProcessMemoryLimit = (SIZE_T) profile->memory_kb * 1024;
	if (!SetInformationJobObject(job,
								 JobObjectExtendedLimitInformation,
								 &job_limits,
								 sizeof(job_limits)))
	{
		CloseHandle(job);
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not configure sandbox job object for principal \"%s\": %m (error code %lu)",
						program_path, GetLastError())));
	}

	timeout_value = psprintf("%d", profile->timeout_ms);
	memory_value = psprintf("%d", profile->memory_kb);
	process_limit_value = psprintf("%d", profile->process_limit);
	nofile_value = psprintf("%d", profile->max_open_files);
	initStringInfo(&environment);
#define QX_APPEND_ENV(name, value) \
	do { \
		appendStringInfo(&environment, "%s=%s", (name), (value)); \
		appendBinaryStringInfo(&environment, "\0", 1); \
	} while (0)
	QX_APPEND_ENV("QX_SANDBOX_PROFILE", profile->name);
	QX_APPEND_ENV("QX_SANDBOX_TIMEOUT_MS", timeout_value);
	QX_APPEND_ENV("QX_SANDBOX_MEMORY_KB", memory_value);
	QX_APPEND_ENV("QX_SANDBOX_PROCESS_LIMIT", process_limit_value);
	QX_APPEND_ENV("QX_SANDBOX_MAX_OPEN_FILES", nofile_value);
	QX_APPEND_ENV("QX_RUNTIME_ROOT", runtime_dir);
	QX_APPEND_ENV("TMP", runtime_dir);
	QX_APPEND_ENV("TEMP", runtime_dir);
	if (system_root != NULL && system_root[0] != '\0')
		QX_APPEND_ENV("SystemRoot", system_root);
	appendBinaryStringInfo(&environment, "\0", 1);
#undef QX_APPEND_ENV
	pfree(timeout_value);
	pfree(memory_value);
	pfree(process_limit_value);
	pfree(nofile_value);

	snprintf(cmdline, sizeof(cmdline),
			 "\"%s\" --request-file \"%s\" --response-file \"%s\"",
			 program_path, request_path, response_path);

	if (need_restricted_identity)
	{
		if (!OpenProcessToken(GetCurrentProcess(),
							  TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY |
							  TOKEN_QUERY | TOKEN_ADJUST_DEFAULT |
							  TOKEN_ADJUST_PRIVILEGES,
							  &base_token))
		{
			CloseHandle(job);
			pfree(environment.data);
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not open process token for principal \"%s\": %m (error code %lu)",
							program_path, GetLastError())));
		}

		if (!CreateRestrictedToken(base_token,
								   DISABLE_MAX_PRIVILEGE | LUA_TOKEN,
								   0, NULL, 0, NULL, 0, NULL,
								   &launch_token))
		{
			CloseHandle(base_token);
			CloseHandle(job);
			pfree(environment.data);
			ereport(ERROR,
					(errcode_for_file_access(),
					 errmsg("could not create restricted token for principal \"%s\": %m (error code %lu)",
							program_path, GetLastError())));
		}
	}

	started_at = GetCurrentTimestamp();
	if (!(need_restricted_identity ?
		  CreateProcessAsUserA(launch_token,
							   program_path,
							   cmdline,
							   NULL,
							   NULL,
							   FALSE,
							   CREATE_SUSPENDED | CREATE_NO_WINDOW,
							   environment.data,
							   runtime_dir,
							   &si,
							   &pi) :
		  CreateProcessA(program_path,
						 cmdline,
						 NULL,
						 NULL,
						 FALSE,
						 CREATE_SUSPENDED | CREATE_NO_WINDOW,
						 environment.data,
						 runtime_dir,
						 &si,
						 &pi)))
	{
		if (launch_token != NULL)
			CloseHandle(launch_token);
		if (base_token != NULL)
			CloseHandle(base_token);
		CloseHandle(job);
		pfree(environment.data);
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not start principal program \"%s\": %m (error code %lu)",
						program_path, GetLastError())));
	}

	if (!AssignProcessToJobObject(job, pi.hProcess))
	{
		TerminateProcess(pi.hProcess, 1);
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
		if (launch_token != NULL)
			CloseHandle(launch_token);
		if (base_token != NULL)
			CloseHandle(base_token);
		CloseHandle(job);
		pfree(environment.data);
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not assign principal program \"%s\" to sandbox job: %m (error code %lu)",
						program_path, GetLastError())));
	}

	if (ResumeThread(pi.hThread) == (DWORD) -1)
	{
		TerminateJobObject(job, 1);
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
		if (launch_token != NULL)
			CloseHandle(launch_token);
		if (base_token != NULL)
			CloseHandle(base_token);
		CloseHandle(job);
		pfree(environment.data);
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not resume principal program \"%s\": %m (error code %lu)",
						program_path, GetLastError())));
	}

	wait_status = WaitForSingleObject(pi.hProcess, profile->timeout_ms);
	if (wait_status == WAIT_TIMEOUT)
	{
		TerminateJobObject(job, 1);
		WaitForSingleObject(pi.hProcess, INFINITE);
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
		if (launch_token != NULL)
			CloseHandle(launch_token);
		if (base_token != NULL)
			CloseHandle(base_token);
		CloseHandle(job);
		pfree(environment.data);
		ereport(ERROR,
				(errcode(ERRCODE_QUERY_CANCELED),
				 errmsg("principal program \"%s\" exceeded sandbox timeout",
						program_path),
				 errdetail("Sandbox profile \"%s\" allows at most %d ms of wall time.",
						   profile->name, profile->timeout_ms)));
	}

	if (wait_status != WAIT_OBJECT_0)
	{
		TerminateJobObject(job, 1);
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
		if (launch_token != NULL)
			CloseHandle(launch_token);
		if (base_token != NULL)
			CloseHandle(base_token);
		CloseHandle(job);
		pfree(environment.data);
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not wait for principal program \"%s\": %m (error code %lu)",
						program_path, GetLastError())));
	}

	if (!GetExitCodeProcess(pi.hProcess, &exit_code))
	{
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
		if (launch_token != NULL)
			CloseHandle(launch_token);
		if (base_token != NULL)
			CloseHandle(base_token);
		CloseHandle(job);
		pfree(environment.data);
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not get exit status for principal program \"%s\": %m (error code %lu)",
						program_path, GetLastError())));
	}

	CloseHandle(pi.hThread);
	CloseHandle(pi.hProcess);
	if (launch_token != NULL)
		CloseHandle(launch_token);
	if (base_token != NULL)
		CloseHandle(base_token);
	CloseHandle(job);
	observation->wall_time_ms =
		(int32) TimestampDifferenceMilliseconds(started_at, GetCurrentTimestamp());
	observation->restricted_identity = need_restricted_identity;
	pfree(environment.data);

	if (exit_code != 0)
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("principal program \"%s\" failed", program_path),
				 errdetail("Exit status was %lu.", exit_code)));
#endif
}

static void
qx_read_external_result(const char *path, QxExternalToolResult *result)
{
	FILE	   *file;
	char		line[1024];

	file = AllocateFile(path, "r");
	if (file == NULL)
		ereport(ERROR,
				(errcode_for_file_access(),
				 errmsg("could not read principal response file \"%s\": %m",
						path)));

	memset(result, 0, sizeof(*result));
	while (fgets(line, sizeof(line), file) != NULL)
	{
		char	   *eq;
		char	   *key;
		char	   *value;

		eq = strchr(line, '=');
		if (eq == NULL)
			continue;
		*eq = '\0';
		key = line;
		value = eq + 1;
		value[strcspn(value, "\r\n")] = '\0';

		if (strcmp(key, "TOKENS") == 0)
			result->token_charge = pg_strtoint32(value);
		else if (strcmp(key, "COST") == 0)
			result->cost_charge = pg_strtoint32(value);
		else if (strcmp(key, "DETAIL") == 0)
			result->detail = pstrdup(value);
		else if (strcmp(key, "TOOL") == 0)
			result->tool_name = pstrdup(value);
		else if (strcmp(key, "PRINCIPAL") == 0)
			result->principal_name = pstrdup(value);
		else if (strcmp(key, "PRINCIPAL_RUNTIME") == 0)
			result->principal_runtime = pstrdup(value);
		else if (strcmp(key, "PROVIDER") == 0)
			result->provider_name = pstrdup(value);
		else if (strcmp(key, "PROVIDER_KIND") == 0)
			result->provider_kind = pstrdup(value);
		else if (strcmp(key, "SANDBOX") == 0)
			result->sandbox_name = pstrdup(value);
		else if (strcmp(key, "PROFILE") == 0)
			result->profile_name = pstrdup(value);
		else if (strcmp(key, "ENV") == 0)
			result->environment_mode = pstrdup(value);
		else if (strcmp(key, "WORKDIR") == 0)
			result->workdir_name = pstrdup(value);
		else if (strcmp(key, "RECEIPT_SCHEMA") == 0)
			result->receipt_schema = pstrdup(value);
		else if (strcmp(key, "RECEIPT_ALG") == 0)
			result->receipt_alg = pstrdup(value);
		else if (strcmp(key, "RECEIPT_NONCE") == 0)
			result->receipt_nonce = pstrdup(value);
		else if (strcmp(key, "RECEIPT_SIG") == 0)
			result->receipt_signature = pstrdup(value);
		else if (strcmp(key, "ATTESTATION") == 0)
			result->attestation_mode = pstrdup(value);
		else if (strcmp(key, "TIMEOUT_MS") == 0)
			result->timeout_ms = pg_strtoint32(value);
		else if (strcmp(key, "PROCESS_LIMIT") == 0)
			result->process_limit = pg_strtoint32(value);
		else if (strcmp(key, "PATH_PRESENT") == 0)
			result->path_present = (strcmp(value, "true") == 0);
		else if (strcmp(key, "STATUS") == 0 && strcmp(value, "ok") != 0)
		{
			FreeFile(file);
			ereport(ERROR,
					(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
					 errmsg("principal response reported status \"%s\"", value)));
		}
	}

	FreeFile(file);

	if (result->detail == NULL)
		result->detail = pstrdup("external tool execution completed");
}

static void
qx_validate_external_result(const QxExternalToolResult *result,
							const QxSandboxProfile *profile,
							const char *phase,
							Oid taskoid,
							const char *expected_tool,
							const char *expected_principal,
							const char *expected_principal_runtime,
							const char *expected_provider,
							const char *expected_provider_kind,
							const char *expected_provider_endpoint,
							const char *expected_receipt_schema,
							const char *expected_receipt_alg,
							const char *expected_receipt_nonce,
							const char *receipt_key,
							bool require_attestation)
{
	char	   *expected_attestation;
	char	   *receipt_payload;

	if (expected_tool != NULL &&
		(result->tool_name == NULL ||
		 strcmp(result->tool_name, expected_tool) != 0))
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("principal response returned tool mismatch"),
				 errdetail("Expected tool \"%s\" but got \"%s\".",
						   expected_tool,
						   result->tool_name != NULL ? result->tool_name : "<null>")));

	if (expected_principal != NULL &&
		(result->principal_name == NULL ||
		 strcmp(result->principal_name, expected_principal) != 0))
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("principal response returned principal mismatch"),
				 errdetail("Expected principal \"%s\" but got \"%s\".",
						   expected_principal,
						   result->principal_name != NULL ? result->principal_name : "<null>")));

	if (expected_principal_runtime != NULL &&
		(result->principal_runtime == NULL ||
		 strcmp(result->principal_runtime, expected_principal_runtime) != 0))
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("principal response returned principal-runtime mismatch"),
				 errdetail("Expected principal runtime \"%s\" but got \"%s\".",
						   expected_principal_runtime,
						   result->principal_runtime != NULL ? result->principal_runtime : "<null>")));

	if (expected_provider != NULL &&
		(result->provider_name == NULL ||
		 strcmp(result->provider_name, expected_provider) != 0))
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("principal response returned provider mismatch"),
				 errdetail("Expected provider \"%s\" but got \"%s\".",
						   expected_provider,
						   result->provider_name != NULL ? result->provider_name : "<null>")));

	if (expected_provider_kind != NULL &&
		(result->provider_kind == NULL ||
		 strcmp(result->provider_kind, expected_provider_kind) != 0))
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("principal response returned provider-kind mismatch"),
				 errdetail("Expected provider kind \"%s\" but got \"%s\".",
						   expected_provider_kind,
						   result->provider_kind != NULL ? result->provider_kind : "<null>")));

	if (result->profile_name == NULL ||
		strcmp(result->profile_name, profile->name) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("principal response returned sandbox profile mismatch"),
				 errdetail("Expected profile \"%s\" but got \"%s\".",
						   profile->name,
						   result->profile_name != NULL ? result->profile_name : "<null>")));

	if (result->environment_mode == NULL ||
		strcmp(result->environment_mode, "minimal") != 0)
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("principal response did not run under a minimal sandbox environment")));

	if (result->workdir_name == NULL ||
		strcmp(result->workdir_name, "pg_qx_runtime") != 0)
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("principal response escaped the runtime workdir"),
				 errdetail("Expected workdir basename \"pg_qx_runtime\" but got \"%s\".",
						   result->workdir_name != NULL ? result->workdir_name : "<null>")));

	if (result->timeout_ms != profile->timeout_ms)
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("principal response reported timeout mismatch"),
				 errdetail("Expected %d ms but got %d ms.",
						   profile->timeout_ms, result->timeout_ms)));

	if (result->process_limit != profile->process_limit)
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("principal response reported process-limit mismatch"),
				 errdetail("Expected %d but got %d.",
						   profile->process_limit, result->process_limit)));

	if (result->path_present)
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("principal response observed PATH in sandbox environment"),
				 errdetail("Stage 17 requires PATH to be stripped from external principal execution.")));

	if (expected_receipt_schema != NULL &&
		(result->receipt_schema == NULL ||
		 strcmp(result->receipt_schema, expected_receipt_schema) != 0))
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("principal response returned receipt schema mismatch"),
				 errdetail("Expected receipt schema \"%s\" but got \"%s\".",
						   expected_receipt_schema,
						   result->receipt_schema != NULL ? result->receipt_schema : "<null>")));

	if (expected_receipt_alg != NULL &&
		(result->receipt_alg == NULL ||
		 strcmp(result->receipt_alg, expected_receipt_alg) != 0))
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("principal response returned receipt algorithm mismatch"),
				 errdetail("Expected receipt algorithm \"%s\" but got \"%s\".",
						   expected_receipt_alg,
						   result->receipt_alg != NULL ? result->receipt_alg : "<null>")));

	if (expected_receipt_nonce != NULL &&
		(result->receipt_nonce == NULL ||
		 strcmp(result->receipt_nonce, expected_receipt_nonce) != 0))
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("principal response returned receipt nonce mismatch"),
				 errdetail("Expected receipt nonce \"%s\" but got \"%s\".",
						   expected_receipt_nonce,
						   result->receipt_nonce != NULL ? result->receipt_nonce : "<null>")));

	expected_attestation = qx_expected_attestation_mode(expected_provider_kind,
														 require_attestation);
	if (result->attestation_mode == NULL ||
		strcmp(result->attestation_mode, expected_attestation) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("principal response failed attestation requirements"),
				 errdetail("Expected attestation mode \"%s\" but got \"%s\".",
						   expected_attestation,
						   result->attestation_mode != NULL ? result->attestation_mode : "<null>")));

	if (result->receipt_signature == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
				 errmsg("principal response omitted a signed receipt"),
				 errdetail("Providers must return RECEIPT_SIG so the runtime can verify execution evidence.")));

	receipt_payload = qx_receipt_payload(phase,
										 taskoid,
										 expected_tool,
										 expected_principal,
										 expected_principal_runtime,
										 expected_provider,
										 expected_provider_kind,
										 expected_provider_endpoint,
										 result->sandbox_name,
										 result->profile_name,
										 result->environment_mode,
										 result->workdir_name,
										 result->timeout_ms,
										 result->process_limit,
										 result->path_present,
										 result->receipt_schema,
										 result->receipt_alg,
										 result->receipt_nonce,
										 result->attestation_mode,
										 result->token_charge,
										 result->cost_charge,
										 result->detail);

	if (expected_receipt_alg != NULL &&
		strcmp(expected_receipt_alg, "ed25519") == 0)
	{
#ifdef USE_OPENSSL
		if (!qx_verify_ed25519_receipt_signature(receipt_key,
												 receipt_payload,
												 result->receipt_signature))
			ereport(ERROR,
					(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
					 errmsg("principal response returned an invalid ed25519 receipt signature")));
#else
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("ed25519 receipt verification requires OpenSSL support")));
#endif
	}
	else
	{
		char	   *expected_signature;

		expected_signature = qx_receipt_hmac_signature_hex(receipt_key,
														   receipt_payload);
		if (strcmp(result->receipt_signature, expected_signature) != 0)
			ereport(ERROR,
					(errcode(ERRCODE_EXTERNAL_ROUTINE_EXCEPTION),
					 errmsg("principal response returned an invalid receipt signature"),
					 errdetail("Expected HMAC-SHA256 signature \"%s\" but got \"%s\".",
							   expected_signature,
							   result->receipt_signature)));
		pfree(expected_signature);
	}
	pfree(expected_attestation);
	pfree(receipt_payload);
}

static void
qx_free_external_result(QxExternalToolResult *result)
{
	if (result->detail != NULL)
		pfree(result->detail);
	if (result->tool_name != NULL)
		pfree(result->tool_name);
	if (result->principal_name != NULL)
		pfree(result->principal_name);
	if (result->principal_runtime != NULL)
		pfree(result->principal_runtime);
	if (result->provider_name != NULL)
		pfree(result->provider_name);
	if (result->provider_kind != NULL)
		pfree(result->provider_kind);
	if (result->sandbox_name != NULL)
		pfree(result->sandbox_name);
	if (result->profile_name != NULL)
		pfree(result->profile_name);
	if (result->environment_mode != NULL)
		pfree(result->environment_mode);
	if (result->workdir_name != NULL)
		pfree(result->workdir_name);
	if (result->receipt_schema != NULL)
		pfree(result->receipt_schema);
	if (result->receipt_alg != NULL)
		pfree(result->receipt_alg);
	if (result->receipt_nonce != NULL)
		pfree(result->receipt_nonce);
	if (result->receipt_signature != NULL)
		pfree(result->receipt_signature);
	if (result->attestation_mode != NULL)
		pfree(result->attestation_mode);
	memset(result, 0, sizeof(*result));
}

static void
qx_execute_tool_contract(const char *contract, const char *phase, Oid taskoid,
						 const char *goal, bool input_present,
						 QxExternalToolResult *result)
{
	char	   *tool_name;
	char	   *handler_name;
	char	   *tool_sandbox;
	char	   *principal_name;
	char	   *principal_sandbox;
	char	   *principal_runtime;
	char	   *program_name;
	char	   *receipt_signer;
	char	   *provider_name;
	char	   *provider_oid_str;
	char	   *provider_kind;
	char	   *provider_endpoint;
	char	   *provider_attestation;
	char	   *receipt_schema;
	char	   *receipt_alg;
	char	   *receipt_nonce;
	char	   *receipt_key;
	char	   *expected_attestation;
	const char *effective_sandbox;
	const QxSandboxProfile *profile;
	QxSandboxObservation observation;
	char		runtime_dir[MAXPGPATH];
	char		program_path[MAXPGPATH];
	char		signer_path[MAXPGPATH];
	char		request_path[MAXPGPATH];
	char		response_path[MAXPGPATH];
	char	   *request_payload;
	Oid			provideroid = InvalidOid;
	const char *resolved_signer = "";

	if (contract == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("task %u has no authorized tool contract to execute",
						taskoid)));

	tool_name = qx_contract_value(contract, "tool");
	handler_name = qx_contract_value(contract, "handler");
	tool_sandbox = qx_contract_value(contract, "tool_sandbox");
	principal_name = qx_contract_value(contract, "principal");
	principal_sandbox = qx_contract_value(contract, "principal_sandbox");
	principal_runtime = qx_contract_value(contract, "principal_runtime");
	program_name = qx_contract_value(contract, "program");
	receipt_signer = qx_contract_value(contract, "receipt_signer");
	provider_name = qx_contract_value(contract, "provider");
	provider_oid_str = qx_contract_value(contract, "provider_oid");
	provider_kind = qx_contract_value(contract, "provider_kind");
	provider_endpoint = qx_contract_value(contract, "provider_endpoint");
	provider_attestation = qx_contract_value(contract, "provider_attestation");
	receipt_schema = qx_contract_value(contract, "receipt_schema");
	receipt_alg = qx_contract_value(contract, "receipt_alg");
	receipt_nonce = psprintf("%s:%s:%s",
							 phase != NULL ? phase : "submit",
							 tool_name != NULL ? tool_name : "tool",
							 principal_name != NULL ? principal_name : "principal");
	if (principal_runtime == NULL || principal_runtime[0] == '\0')
	{
		if (principal_runtime != NULL)
			pfree(principal_runtime);
		principal_runtime =
			pstrdup(qx_default_runtime_for_provider_kind(provider_kind));
	}
	expected_attestation = qx_expected_attestation_mode(provider_kind,
														 provider_attestation != NULL &&
														 strcmp(provider_attestation, "required") == 0);

	if (provider_oid_str == NULL || provider_oid_str[0] == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("tool contract is missing provider_oid for receipt verification")));
	provideroid = (Oid) strtoul(provider_oid_str, NULL, 10);
	if (!OidIsValid(provideroid))
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("tool contract carried an invalid provider_oid \"%s\"",
						provider_oid_str)));
	receipt_key = qx_provider_receipt_key(provideroid);
	if (receipt_alg != NULL && strcmp(receipt_alg, "ed25519") == 0)
	{
		if (receipt_signer == NULL || receipt_signer[0] == '\0')
			ereport(ERROR,
					(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
					 errmsg("tool contract is missing receipt_signer for ed25519 verification")));
		qx_resolve_receipt_signer_path(receipt_signer,
									   signer_path,
									   sizeof(signer_path));
		resolved_signer = signer_path;
	}

	if (qx_sandbox_rank(tool_sandbox) > qx_sandbox_rank(principal_sandbox))
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("tool \"%s\" exceeds principal \"%s\" sandbox ceiling",
						tool_name != NULL ? tool_name : "<unknown>",
						principal_name != NULL ? principal_name : "<unknown>")));

	effective_sandbox = qx_effective_sandbox_name(tool_sandbox,
												  principal_sandbox);
	profile = qx_lookup_sandbox_profile(effective_sandbox);
	qx_resolve_principal_program_path(program_name, program_path, sizeof(program_path));
	qx_runtime_temp_dir(runtime_dir, sizeof(runtime_dir));
	qx_runtime_temp_path(request_path, sizeof(request_path), "req");
	qx_runtime_temp_path(response_path, sizeof(response_path), "resp");

	request_payload = psprintf(
		"PHASE=%s\nTASK_OID=%u\nGOAL_LENGTH=%zu\nINPUT_PRESENT=%s\nTOOL=%s\nHANDLER=%s\nSANDBOX=%s\nPRINCIPAL=%s\nPRINCIPAL_RUNTIME=%s\nPROVIDER=%s\nPROVIDER_KIND=%s\nPROVIDER_ENDPOINT=%s\nREQUIRE_ATTESTATION=%s\nRECEIPT_SCHEMA=%s\nRECEIPT_ALG=%s\nRECEIPT_NONCE=%s\nRECEIPT_KEY=%s\nRECEIPT_SIGNER=%s\n",
		phase,
		taskoid,
		goal != NULL ? strlen(goal) : 0,
		input_present ? "true" : "false",
		tool_name != NULL ? tool_name : "",
		handler_name != NULL ? handler_name : "",
		tool_sandbox != NULL ? tool_sandbox : "builtin",
		principal_name != NULL ? principal_name : "",
		principal_runtime != NULL ? principal_runtime : "host",
		provider_name != NULL ? provider_name : "",
		provider_kind != NULL ? provider_kind : "loopback",
		provider_endpoint != NULL ? provider_endpoint : "local://qhapaqxian-tool-runner",
		(provider_attestation != NULL && strcmp(provider_attestation, "required") == 0) ? "true" : "false",
		receipt_schema != NULL ? receipt_schema : "qx.receipt.v1",
		receipt_alg != NULL ? receipt_alg : "hmac-sha256",
		receipt_nonce,
		(receipt_alg != NULL && strcmp(receipt_alg, "ed25519") == 0) ? "" : receipt_key,
		resolved_signer);
	qx_write_text_file(request_path, request_payload);
	pfree(request_payload);

	memset(&observation, 0, sizeof(observation));
	qx_launch_principal_program(program_path, profile, runtime_dir,
								request_path, response_path,
								&observation);
	qx_read_external_result(response_path, result);
	qx_validate_external_result(result, profile,
								phase,
								taskoid,
								tool_name,
								principal_name,
								principal_runtime != NULL ? principal_runtime : "host",
								provider_name,
								provider_kind != NULL ? provider_kind : "loopback",
								provider_endpoint != NULL ? provider_endpoint : "local://qhapaqxian-tool-runner",
								receipt_schema != NULL ? receipt_schema : "qx.receipt.v1",
								receipt_alg != NULL ? receipt_alg : "hmac-sha256",
								receipt_nonce,
								receipt_key,
								provider_attestation != NULL &&
								strcmp(provider_attestation, "required") == 0);

	if (unlink(request_path) != 0 && errno != ENOENT)
		elog(WARNING, "could not remove QhapaqXian request file \"%s\": %m",
			 request_path);
	if (unlink(response_path) != 0 && errno != ENOENT)
		elog(WARNING, "could not remove QhapaqXian response file \"%s\": %m",
			 response_path);

	if (result->tool_name == NULL && tool_name != NULL)
		result->tool_name = pstrdup(tool_name);
	if (result->principal_name == NULL && principal_name != NULL)
		result->principal_name = pstrdup(principal_name);
	if (result->principal_runtime == NULL)
		result->principal_runtime = pstrdup(principal_runtime != NULL ? principal_runtime : "host");
	if (result->provider_name == NULL && provider_name != NULL)
		result->provider_name = pstrdup(provider_name);
	if (result->provider_kind == NULL)
		result->provider_kind = pstrdup(provider_kind != NULL ? provider_kind : "loopback");
	if (result->sandbox_name == NULL)
		result->sandbox_name = pstrdup(effective_sandbox);
	if (result->profile_name == NULL)
		result->profile_name = pstrdup(profile->name);
	if (result->environment_mode == NULL)
		result->environment_mode = pstrdup("minimal");
	if (result->workdir_name == NULL)
		result->workdir_name = pstrdup("pg_qx_runtime");
	if (result->receipt_schema == NULL)
		result->receipt_schema = pstrdup(receipt_schema != NULL ? receipt_schema : "qx.receipt.v1");
	if (result->receipt_alg == NULL)
		result->receipt_alg = pstrdup(receipt_alg != NULL ? receipt_alg : "hmac-sha256");
	if (result->receipt_nonce == NULL)
		result->receipt_nonce = pstrdup(receipt_nonce);
	if (result->attestation_mode == NULL)
		result->attestation_mode = expected_attestation;
	else
		pfree(expected_attestation);
	if (result->receipt_signature == NULL)
		result->receipt_signature = pstrdup("verified");
	if (result->timeout_ms == 0)
		result->timeout_ms = profile->timeout_ms;
	if (result->process_limit == 0)
		result->process_limit = profile->process_limit;
	if (result->detail != NULL)
	{
		char	   *augmented_detail;

		augmented_detail = psprintf("%s;launch_mode=%s;restricted_identity=%s;wall_ms=%d",
									result->detail,
									observation.launch_mode != NULL ? observation.launch_mode : "profiled_process",
									observation.restricted_identity ? "true" : "false",
									observation.wall_time_ms);
		pfree(result->detail);
		result->detail = augmented_detail;
	}

	if (receipt_signer != NULL)
		pfree(receipt_signer);

	if (tool_name != NULL)
		pfree(tool_name);
	if (handler_name != NULL)
		pfree(handler_name);
	if (tool_sandbox != NULL)
		pfree(tool_sandbox);
	if (principal_name != NULL)
		pfree(principal_name);
	if (principal_sandbox != NULL)
		pfree(principal_sandbox);
	if (principal_runtime != NULL)
		pfree(principal_runtime);
	if (program_name != NULL)
		pfree(program_name);
	if (provider_name != NULL)
		pfree(provider_name);
	if (provider_oid_str != NULL)
		pfree(provider_oid_str);
	if (provider_kind != NULL)
		pfree(provider_kind);
	if (provider_endpoint != NULL)
		pfree(provider_endpoint);
	if (provider_attestation != NULL)
		pfree(provider_attestation);
	if (receipt_schema != NULL)
		pfree(receipt_schema);
	if (receipt_alg != NULL)
		pfree(receipt_alg);
	if (receipt_nonce != NULL)
		pfree(receipt_nonce);
	if (receipt_key != NULL)
		pfree(receipt_key);
}

static List *
qx_fetch_task_authorized_contracts(HeapTuple tasktup)
{
	char	   *serialized;
	Node	   *node;

	serialized = qx_text_attr_from_syscache(tasktup,
											Anum_pg_qx_task_qxtaskauthorizedtools,
											QXTASKOID);
	if (serialized == NULL)
		return NIL;

	node = stringToNode(serialized);
	pfree(serialized);
	if (node == NULL)
		return NIL;
	if (!IsA(node, List))
		elog(ERROR, "QhapaqXian authorized tool payload was not a List");

	return castNode(List, node);
}

static char *
qx_fetch_task_goal(HeapTuple tasktup)
{
	return qx_text_attr_from_syscache(tasktup,
									  Anum_pg_qx_task_qxtaskgoal,
									  QXTASKOID);
}

static bool
qx_task_input_present(HeapTuple tasktup)
{
	bool		isnull;

	(void) SysCacheGetAttr(QXTASKOID, tasktup,
						   Anum_pg_qx_task_qxtaskinput,
						   &isnull);
	return !isnull;
}

static void
qx_update_task_runtime(Relation taskrel, Oid taskoid, char state,
					   Oid lastattemptid, bool replace_attempt,
					   Oid lastcheckpointid, bool replace_checkpoint)
{
	HeapTuple	tasktup;
	HeapTuple	newtup;
	Datum		values[Natts_pg_qx_task];
	bool		nulls[Natts_pg_qx_task];
	bool		replaces[Natts_pg_qx_task];

	tasktup = SearchSysCache1(QXTASKOID, ObjectIdGetDatum(taskoid));
	if (!HeapTupleIsValid(tasktup))
		elog(ERROR, "cache lookup failed for QhapaqXian task %u", taskoid);

	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));
	memset(replaces, false, sizeof(replaces));

	values[Anum_pg_qx_task_qxtaskstate - 1] = CharGetDatum(state);
	replaces[Anum_pg_qx_task_qxtaskstate - 1] = true;

	if (replace_attempt)
	{
		values[Anum_pg_qx_task_qxtasklastattemptid - 1] =
			ObjectIdGetDatum(lastattemptid);
		replaces[Anum_pg_qx_task_qxtasklastattemptid - 1] = true;
	}

	if (replace_checkpoint)
	{
		values[Anum_pg_qx_task_qxtasklastcheckpointid - 1] =
			ObjectIdGetDatum(lastcheckpointid);
		replaces[Anum_pg_qx_task_qxtasklastcheckpointid - 1] = true;
	}

	newtup = heap_modify_tuple(tasktup, RelationGetDescr(taskrel),
							   values, nulls, replaces);
	CatalogTupleUpdate(taskrel, &tasktup->t_self, newtup);

	heap_freetuple(newtup);
	ReleaseSysCache(tasktup);
	CommandCounterIncrement();
}

static void
qx_update_attempt_state(Relation attemptrel, Oid attemptoid, char state)
{
	HeapTuple	attempttup;
	HeapTuple	newtup;
	Datum		values[Natts_pg_qx_attempt];
	bool		nulls[Natts_pg_qx_attempt];
	bool		replaces[Natts_pg_qx_attempt];

	attempttup = SearchSysCache1(QXATTEMPTOID, ObjectIdGetDatum(attemptoid));
	if (!HeapTupleIsValid(attempttup))
		elog(ERROR, "cache lookup failed for QhapaqXian attempt %u", attemptoid);

	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));
	memset(replaces, false, sizeof(replaces));

	values[Anum_pg_qx_attempt_qxattemptstate - 1] = CharGetDatum(state);
	replaces[Anum_pg_qx_attempt_qxattemptstate - 1] = true;

	newtup = heap_modify_tuple(attempttup, RelationGetDescr(attemptrel),
							   values, nulls, replaces);
	CatalogTupleUpdate(attemptrel, &attempttup->t_self, newtup);

	heap_freetuple(newtup);
	ReleaseSysCache(attempttup);
	CommandCounterIncrement();
}

static void
qx_charge_task_budget(Relation taskrel, Oid taskoid,
					  int32 token_delta, int32 cost_delta,
					  const char *charge_name)
{
	HeapTuple	tasktup;
	HeapTuple	newtup;
	Form_pg_qx_task taskform;
	Datum		values[Natts_pg_qx_task];
	bool		nulls[Natts_pg_qx_task];
	bool		replaces[Natts_pg_qx_task];
	int32		new_tokens;
	int32		new_cost;

	tasktup = SearchSysCache1(QXTASKOID, ObjectIdGetDatum(taskoid));
	if (!HeapTupleIsValid(tasktup))
		elog(ERROR, "cache lookup failed for QhapaqXian task %u", taskoid);

	taskform = (Form_pg_qx_task) GETSTRUCT(tasktup);
	new_tokens = taskform->qxtaskconsumedtokens + token_delta;
	new_cost = taskform->qxtaskconsumedcost + cost_delta;

	if (taskform->qxtaskbudgettokens > 0 && new_tokens > taskform->qxtaskbudgettokens)
	{
		ReleaseSysCache(tasktup);
		ereport(ERROR,
				(errcode(ERRCODE_CONFIGURATION_LIMIT_EXCEEDED),
				 errmsg("runtime token budget exceeded for task %u", taskoid),
				 errdetail("Charge \"%s\" would move token usage to %d, above the ceiling %d.",
						   charge_name, new_tokens, taskform->qxtaskbudgettokens)));
	}

	if (taskform->qxtaskbudgetcost > 0 && new_cost > taskform->qxtaskbudgetcost)
	{
		ReleaseSysCache(tasktup);
		ereport(ERROR,
				(errcode(ERRCODE_CONFIGURATION_LIMIT_EXCEEDED),
				 errmsg("runtime cost budget exceeded for task %u", taskoid),
				 errdetail("Charge \"%s\" would move cost usage to %d, above the ceiling %d.",
						   charge_name, new_cost, taskform->qxtaskbudgetcost)));
	}

	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));
	memset(replaces, false, sizeof(replaces));

	values[Anum_pg_qx_task_qxtaskconsumedtokens - 1] = Int32GetDatum(new_tokens);
	values[Anum_pg_qx_task_qxtaskconsumedcost - 1] = Int32GetDatum(new_cost);
	replaces[Anum_pg_qx_task_qxtaskconsumedtokens - 1] = true;
	replaces[Anum_pg_qx_task_qxtaskconsumedcost - 1] = true;

	newtup = heap_modify_tuple(tasktup, RelationGetDescr(taskrel),
							   values, nulls, replaces);
	CatalogTupleUpdate(taskrel, &tasktup->t_self, newtup);

	heap_freetuple(newtup);
	ReleaseSysCache(tasktup);
	CommandCounterIncrement();
}

static Oid
qx_insert_attempt(Relation rel, Oid sessionoid, Oid taskoid, Oid ownerid,
				  Oid resumecheckpointid, int16 seqno, char state,
				  const char *strategy)
{
	Datum		values[Natts_pg_qx_attempt];
	bool		nulls[Natts_pg_qx_attempt];
	Oid			attemptoid;
	HeapTuple	tup;

	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	attemptoid = GetNewOidWithIndex(rel, QxAttemptOidIndexId,
									Anum_pg_qx_attempt_oid);
	values[Anum_pg_qx_attempt_oid - 1] = ObjectIdGetDatum(attemptoid);
	values[Anum_pg_qx_attempt_qxattemptdbid - 1] =
		ObjectIdGetDatum(MyDatabaseId);
	values[Anum_pg_qx_attempt_qxattemptsessionid - 1] =
		ObjectIdGetDatum(sessionoid);
	values[Anum_pg_qx_attempt_qxattempttaskid - 1] = ObjectIdGetDatum(taskoid);
	values[Anum_pg_qx_attempt_qxattemptowner - 1] = ObjectIdGetDatum(ownerid);
	values[Anum_pg_qx_attempt_qxattemptresumecheckpointid - 1] =
		ObjectIdGetDatum(resumecheckpointid);
	values[Anum_pg_qx_attempt_qxattemptseqno - 1] = Int16GetDatum(seqno);
	values[Anum_pg_qx_attempt_qxattemptstate - 1] = CharGetDatum(state);
	qx_set_text_datum(values, nulls, Anum_pg_qx_attempt_qxattemptstrategy,
					  strategy);

	tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);
	CatalogTupleInsert(rel, tup);
	heap_freetuple(tup);

	return attemptoid;
}

static Oid
qx_insert_step(Relation rel, Oid sessionoid, Oid taskoid, int16 seqno,
			   const char *name, const char *detail)
{
	Datum		values[Natts_pg_qx_step];
	bool		nulls[Natts_pg_qx_step];
	Oid			stepoid;
	HeapTuple	tup;

	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	stepoid = GetNewOidWithIndex(rel, QxStepOidIndexId,
								 Anum_pg_qx_step_oid);
	values[Anum_pg_qx_step_oid - 1] = ObjectIdGetDatum(stepoid);
	values[Anum_pg_qx_step_qxstepdbid - 1] = ObjectIdGetDatum(MyDatabaseId);
	values[Anum_pg_qx_step_qxstepsessionid - 1] = ObjectIdGetDatum(sessionoid);
	values[Anum_pg_qx_step_qxsteptaskid - 1] = ObjectIdGetDatum(taskoid);
	values[Anum_pg_qx_step_qxstepseqno - 1] = Int16GetDatum(seqno);
	values[Anum_pg_qx_step_qxstepstate - 1] =
		CharGetDatum(QX_STEP_STATE_COMPLETED);
	qx_set_text_datum(values, nulls, Anum_pg_qx_step_qxstepname, name);
	qx_set_text_datum(values, nulls, Anum_pg_qx_step_qxstepdetail, detail);

	tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);
	CatalogTupleInsert(rel, tup);
	heap_freetuple(tup);

	return stepoid;
}

static Oid
qx_insert_event(Relation rel, Oid sessionoid, Oid taskoid, Oid stepoid,
				Oid ownerid, const char *kind, const char *payload)
{
	Datum		values[Natts_pg_qx_event];
	bool		nulls[Natts_pg_qx_event];
	Oid			eventoid;
	HeapTuple	tup;
	XLogRecPtr	eventlsn;

	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	eventoid = GetNewOidWithIndex(rel, QxEventOidIndexId,
								  Anum_pg_qx_event_oid);
	values[Anum_pg_qx_event_oid - 1] = ObjectIdGetDatum(eventoid);
	values[Anum_pg_qx_event_qxeventdbid - 1] = ObjectIdGetDatum(MyDatabaseId);
	values[Anum_pg_qx_event_qxeventsessionid - 1] =
		ObjectIdGetDatum(sessionoid);
	values[Anum_pg_qx_event_qxeventtaskid - 1] = ObjectIdGetDatum(taskoid);
	values[Anum_pg_qx_event_qxeventstepid - 1] = ObjectIdGetDatum(stepoid);
	values[Anum_pg_qx_event_qxeventowner - 1] = ObjectIdGetDatum(ownerid);
	eventlsn = QxEmitSemanticEventRecord(eventoid, sessionoid, taskoid, stepoid,
										 ownerid, kind, payload);
	values[Anum_pg_qx_event_qxeventlsn - 1] = LSNGetDatum(eventlsn);
	qx_set_text_datum(values, nulls, Anum_pg_qx_event_qxeventkind, kind);
	qx_set_text_datum(values, nulls, Anum_pg_qx_event_qxeventpayload, payload);

	tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);
	CatalogTupleInsert(rel, tup);
	heap_freetuple(tup);

	return eventoid;
}

static Oid
qx_insert_trace(Relation rel, Oid sessionoid, Oid taskoid, Oid stepoid,
				Oid ownerid, const char *name, const char *detail)
{
	Datum		values[Natts_pg_qx_trace];
	bool		nulls[Natts_pg_qx_trace];
	Oid			traceoid;
	HeapTuple	tup;
	XLogRecPtr	tracelsn;

	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	traceoid = GetNewOidWithIndex(rel, QxTraceOidIndexId,
								  Anum_pg_qx_trace_oid);
	values[Anum_pg_qx_trace_oid - 1] = ObjectIdGetDatum(traceoid);
	values[Anum_pg_qx_trace_qxtracedbid - 1] = ObjectIdGetDatum(MyDatabaseId);
	values[Anum_pg_qx_trace_qxtracesessionid - 1] =
		ObjectIdGetDatum(sessionoid);
	values[Anum_pg_qx_trace_qxtracetaskid - 1] = ObjectIdGetDatum(taskoid);
	values[Anum_pg_qx_trace_qxtracestepid - 1] = ObjectIdGetDatum(stepoid);
	values[Anum_pg_qx_trace_qxtraceowner - 1] = ObjectIdGetDatum(ownerid);
	values[Anum_pg_qx_trace_qxtracestate - 1] =
		CharGetDatum(QX_TRACE_STATE_CLOSED);
	tracelsn = QxEmitSemanticTraceRecord(traceoid, sessionoid, taskoid, stepoid,
										 ownerid, QX_TRACE_STATE_CLOSED,
										 name, detail);
	values[Anum_pg_qx_trace_qxtracelsn - 1] = LSNGetDatum(tracelsn);
	qx_set_text_datum(values, nulls, Anum_pg_qx_trace_qxtracename, name);
	qx_set_text_datum(values, nulls, Anum_pg_qx_trace_qxtracedetail, detail);

	tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);
	CatalogTupleInsert(rel, tup);
	heap_freetuple(tup);

	return traceoid;
}

static Oid
qx_insert_checkpoint(Relation rel, Oid sessionoid, Oid taskoid, Oid attemptoid,
					 Oid stepoid, Oid ownerid, char taskstate,
					 int16 nextstepseqno, const char *label, const char *data)
{
	Datum		values[Natts_pg_qx_checkpoint];
	bool		nulls[Natts_pg_qx_checkpoint];
	Oid			checkpointoid;
	HeapTuple	tup;
	XLogRecPtr	checkpointlsn;

	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	checkpointoid = GetNewOidWithIndex(rel, QxCheckpointOidIndexId,
									   Anum_pg_qx_checkpoint_oid);
	values[Anum_pg_qx_checkpoint_oid - 1] = ObjectIdGetDatum(checkpointoid);
	values[Anum_pg_qx_checkpoint_qxcheckpointdbid - 1] =
		ObjectIdGetDatum(MyDatabaseId);
	values[Anum_pg_qx_checkpoint_qxcheckpointsessionid - 1] =
		ObjectIdGetDatum(sessionoid);
	values[Anum_pg_qx_checkpoint_qxcheckpointtaskid - 1] =
		ObjectIdGetDatum(taskoid);
	values[Anum_pg_qx_checkpoint_qxcheckpointattemptid - 1] =
		ObjectIdGetDatum(attemptoid);
	values[Anum_pg_qx_checkpoint_qxcheckpointstepid - 1] =
		ObjectIdGetDatum(stepoid);
	values[Anum_pg_qx_checkpoint_qxcheckpointowner - 1] =
		ObjectIdGetDatum(ownerid);
	values[Anum_pg_qx_checkpoint_qxcheckpointstate - 1] =
		CharGetDatum(QX_CHECKPOINT_STATE_DURABLE);
	values[Anum_pg_qx_checkpoint_qxcheckpointtaskstate - 1] =
		CharGetDatum(taskstate);
	values[Anum_pg_qx_checkpoint_qxcheckpointnextstepseqno - 1] =
		Int16GetDatum(nextstepseqno);
	checkpointlsn = QxEmitSemanticCheckpointRecord(checkpointoid, sessionoid,
												   taskoid, attemptoid, stepoid,
												   ownerid,
												   QX_CHECKPOINT_STATE_DURABLE,
												   taskstate, nextstepseqno,
												   label, data);
	values[Anum_pg_qx_checkpoint_qxcheckpointlsn - 1] =
		LSNGetDatum(checkpointlsn);
	qx_set_text_datum(values, nulls, Anum_pg_qx_checkpoint_qxcheckpointlabel,
					  label);
	qx_set_text_datum(values, nulls, Anum_pg_qx_checkpoint_qxcheckpointdata,
					  data);

	tup = heap_form_tuple(RelationGetDescr(rel), values, nulls);
	CatalogTupleInsert(rel, tup);
	heap_freetuple(tup);

	return checkpointoid;
}

static Oid
qx_runtime_insert_task(Relation taskrel, const QxRuntimeTaskRequest *request)
{
	Datum		values[Natts_pg_qx_task];
	bool		nulls[Natts_pg_qx_task];
	HeapTuple	tup;
	Oid			taskoid;
	ObjectAddress myself;
	ObjectAddress referenced;

	memset(values, 0, sizeof(values));
	memset(nulls, false, sizeof(nulls));

	taskoid = GetNewOidWithIndex(taskrel, QxTaskOidIndexId,
								 Anum_pg_qx_task_oid);
	values[Anum_pg_qx_task_oid - 1] = ObjectIdGetDatum(taskoid);
	values[Anum_pg_qx_task_qxtaskdbid - 1] = ObjectIdGetDatum(MyDatabaseId);
	values[Anum_pg_qx_task_qxtasksessionid - 1] =
		ObjectIdGetDatum(request->sessionoid);
	values[Anum_pg_qx_task_qxtaskagentid - 1] =
		ObjectIdGetDatum(request->agentoid);
	values[Anum_pg_qx_task_qxtasknamespacepolicyid - 1] =
		ObjectIdGetDatum(request->namespace_policy_oid);
	values[Anum_pg_qx_task_qxtaskidentityid - 1] =
		ObjectIdGetDatum(request->identityoid);
	values[Anum_pg_qx_task_qxtaskowner - 1] =
		ObjectIdGetDatum(request->ownerid);
	values[Anum_pg_qx_task_qxtasklastattemptid - 1] =
		ObjectIdGetDatum(InvalidOid);
	values[Anum_pg_qx_task_qxtasklastcheckpointid - 1] =
		ObjectIdGetDatum(InvalidOid);
	values[Anum_pg_qx_task_qxtaskbudgettokens - 1] =
		Int32GetDatum(request->budget_tokens);
	values[Anum_pg_qx_task_qxtaskbudgetcost - 1] =
		Int32GetDatum(request->budget_cost);
	values[Anum_pg_qx_task_qxtaskauthorizedtooltokens - 1] =
		Int32GetDatum(request->authorized_tool_tokens);
	values[Anum_pg_qx_task_qxtaskauthorizedtoolcost - 1] =
		Int32GetDatum(request->authorized_tool_cost);
	values[Anum_pg_qx_task_qxtaskestimatedtokens - 1] =
		Int32GetDatum(request->estimated_tokens);
	values[Anum_pg_qx_task_qxtaskestimatedcost - 1] =
		Int32GetDatum(request->estimated_cost);
	values[Anum_pg_qx_task_qxtaskstate - 1] =
		CharGetDatum(QX_TASK_STATE_QUEUED);
	qx_set_text_datum(values, nulls, Anum_pg_qx_task_qxtaskname,
					  request->task_name);
	qx_set_text_datum(values, nulls, Anum_pg_qx_task_qxtaskgoal,
					  request->goal);
	qx_set_nodetree_datum(values, nulls, Anum_pg_qx_task_qxtaskinput,
						  request->input);
	qx_set_text_datum(values, nulls, Anum_pg_qx_task_qxtaskpriority,
					  request->priority);
	qx_set_nodetree_datum(values, nulls,
						  Anum_pg_qx_task_qxtaskauthorizedtools,
						  request->authorized_tools);

	tup = heap_form_tuple(RelationGetDescr(taskrel), values, nulls);
	CatalogTupleInsert(taskrel, tup);
	heap_freetuple(tup);

	ObjectAddressSet(myself, QxTaskRelationId, taskoid);
	recordDependencyOnOwner(QxTaskRelationId, taskoid, request->ownerid);
	ObjectAddressSet(referenced, QxSessionRelationId, request->sessionoid);
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	ObjectAddressSet(referenced, QxAgentRelationId, request->agentoid);
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	ObjectAddressSet(referenced, QxNamespaceRelationId, request->namespace_policy_oid);
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	ObjectAddressSet(referenced, QxIdentityRelationId, request->identityoid);
	recordDependencyOn(&myself, &referenced, DEPENDENCY_NORMAL);
	recordDependencyOnCurrentExtension(&myself, false);
	InvokeObjectPostCreateHook(QxTaskRelationId, taskoid, 0);

	return taskoid;
}

static char *
qx_fetch_checkpoint_label(HeapTuple checkpointtup)
{
	bool		isnull;
	Datum		datum;

	datum = SysCacheGetAttr(QXCHECKPOINTOID, checkpointtup,
							Anum_pg_qx_checkpoint_qxcheckpointlabel,
							&isnull);
	if (isnull)
		return NULL;

	return TextDatumGetCString(datum);
}

static int16
qx_fetch_attempt_seqno(Oid attemptoid)
{
	HeapTuple	attempttup;
	Form_pg_qx_attempt attemptform;
	int16		seqno;

	attempttup = SearchSysCache1(QXATTEMPTOID, ObjectIdGetDatum(attemptoid));
	if (!HeapTupleIsValid(attempttup))
		elog(ERROR, "cache lookup failed for QhapaqXian attempt %u", attemptoid);

	attemptform = (Form_pg_qx_attempt) GETSTRUCT(attempttup);
	seqno = attemptform->qxattemptseqno;
	ReleaseSysCache(attempttup);

	return seqno;
}

Oid
QxRuntimeSubmitTask(const QxRuntimeTaskRequest *request)
{
	Relation	taskrel;
	Relation	attemptrel;
	Relation	steprel;
	Relation	eventrel;
	Relation	tracerel;
	Relation	checkpointrel;
	Oid			taskoid;
	Oid			attemptoid;
	Oid			stepoid;
	Oid			checkpointoid;
	const char *selected_contract;
	char	   *payload;
	char	   *checkpoint_data;
	QxExternalToolResult tool_result;

	taskrel = table_open(QxTaskRelationId, RowExclusiveLock);
	attemptrel = table_open(QxAttemptRelationId, RowExclusiveLock);
	steprel = table_open(QxStepRelationId, RowExclusiveLock);
	eventrel = table_open(QxEventRelationId, RowExclusiveLock);
	tracerel = table_open(QxTraceRelationId, RowExclusiveLock);
	checkpointrel = table_open(QxCheckpointRelationId, RowExclusiveLock);

	taskoid = qx_runtime_insert_task(taskrel, request);
	CommandCounterIncrement();

	attemptoid = qx_insert_attempt(attemptrel,
								   request->sessionoid,
								   taskoid,
								   request->ownerid,
								   InvalidOid,
								   1,
								   QX_ATTEMPT_STATE_RUNNING,
								   "initial");
	CommandCounterIncrement();

	payload = psprintf("goal=%s;priority=%s",
					   request->goal,
					   request->priority != NULL ? request->priority : "normal");
	(void) qx_insert_event(eventrel, request->sessionoid, taskoid, InvalidOid,
						   request->ownerid, "TASK_QUEUED", payload);
	(void) qx_insert_trace(tracerel, request->sessionoid, taskoid, InvalidOid,
						   request->ownerid, "runtime.queue", payload);
	pfree(payload);

	qx_update_task_runtime(taskrel, taskoid, QX_TASK_STATE_RUNNING,
						   attemptoid, true, InvalidOid, false);

	stepoid = qx_insert_step(steprel, request->sessionoid, taskoid, 1,
							 "stage8.scheduler_admit",
							 "Embedded scheduler admitted attempt 1 into runtime");
	qx_charge_task_budget(taskrel, taskoid, 16, 17, "scheduler_admit");
	payload = psprintf("attempt_opened;state=%c;identity=%s;namespace_policy=%s;tools=%d;budget_cost=%d;budget_tokens=%d",
					   QX_TASK_STATE_RUNNING,
					   request->identity_name != NULL ? request->identity_name : "<unknown>",
					   request->namespace_policy_name != NULL ? request->namespace_policy_name : "<unknown>",
					   list_length(request->authorized_tools),
					   request->budget_cost,
					   request->budget_tokens);
	(void) qx_insert_event(eventrel, request->sessionoid, taskoid, stepoid,
						   request->ownerid, "TASK_DISPATCHED", payload);
	(void) qx_insert_trace(tracerel, request->sessionoid, taskoid, stepoid,
						   request->ownerid, "runtime.dispatch", payload);
	pfree(payload);

	stepoid = qx_insert_step(steprel, request->sessionoid, taskoid, 2,
							 "stage15.authorize_tools",
							 "Attempt 1 validated the runtime tool allowlist against namespace policy");
	payload = psprintf("namespace_policy=%s;authorized_tools=%d;tool_tokens=%d;tool_cost=%d",
					   request->namespace_policy_name != NULL ? request->namespace_policy_name : "<unknown>",
					   list_length(request->authorized_tools),
					   request->authorized_tool_tokens,
					   request->authorized_tool_cost);
	(void) qx_insert_event(eventrel, request->sessionoid, taskoid, stepoid,
						   request->ownerid, "TASK_TOOLS_AUTHORIZED", payload);
	(void) qx_insert_trace(tracerel, request->sessionoid, taskoid, stepoid,
						   request->ownerid, "runtime.authorize_tools", payload);
	pfree(payload);

	stepoid = qx_insert_step(steprel, request->sessionoid, taskoid, 3,
							 "stage16.external_submit",
							 "Attempt 1 executed the selected tool through its principal program");
	selected_contract = request->authorized_tools != NIL ?
		strVal((Node *) linitial(request->authorized_tools)) : NULL;
	qx_execute_tool_contract(selected_contract, "submit", taskoid,
							 request->goal, request->input != NULL,
							 &tool_result);
	qx_charge_task_budget(taskrel, taskoid,
						  tool_result.token_charge,
						  tool_result.cost_charge,
						  "external_submit");
	payload = psprintf("phase=submit;tool=%s;principal=%s;principal_runtime=%s;provider=%s;provider_kind=%s;effective_sandbox=%s;profile=%s;env=%s;cwd=%s;process_limit=%d;timeout_ms=%d;receipt_schema=%s;receipt_alg=%s;receipt_nonce=%s;receipt_sig=%s;attestation=%s;tokens=%d;cost=%d;detail=%s",
					   tool_result.tool_name != NULL ? tool_result.tool_name : "<unknown>",
					   tool_result.principal_name != NULL ? tool_result.principal_name : "<unknown>",
					   tool_result.principal_runtime != NULL ? tool_result.principal_runtime : "<unknown>",
					   tool_result.provider_name != NULL ? tool_result.provider_name : "<unknown>",
					   tool_result.provider_kind != NULL ? tool_result.provider_kind : "<unknown>",
					   tool_result.sandbox_name != NULL ? tool_result.sandbox_name : "<unknown>",
					   tool_result.profile_name != NULL ? tool_result.profile_name : "<unknown>",
					   tool_result.environment_mode != NULL ? tool_result.environment_mode : "<unknown>",
					   tool_result.workdir_name != NULL ? tool_result.workdir_name : "<unknown>",
					   tool_result.process_limit,
					   tool_result.timeout_ms,
					   tool_result.receipt_schema != NULL ? tool_result.receipt_schema : "<unknown>",
					   tool_result.receipt_alg != NULL ? tool_result.receipt_alg : "<unknown>",
					   tool_result.receipt_nonce != NULL ? tool_result.receipt_nonce : "<unknown>",
					   tool_result.receipt_signature != NULL ? "verified" : "missing",
					   tool_result.attestation_mode != NULL ? tool_result.attestation_mode : "<unknown>",
					   tool_result.token_charge,
					   tool_result.cost_charge,
					   tool_result.detail != NULL ? tool_result.detail : "<none>");
	(void) qx_insert_event(eventrel, request->sessionoid, taskoid, stepoid,
						   request->ownerid, "TASK_TOOL_EXECUTED", payload);
	(void) qx_insert_trace(tracerel, request->sessionoid, taskoid, stepoid,
						   request->ownerid, "runtime.external_submit", payload);
	pfree(payload);

	stepoid = qx_insert_step(steprel, request->sessionoid, taskoid, 4,
							 "stage8.capture_input",
							 "Attempt 1 captured task goal and raw input");
	qx_charge_task_budget(taskrel, taskoid,
						  request->input != NULL ? 24 : 8,
						  13,
						  "capture_input");
	payload = psprintf("input_present=%s;task_name=%s",
					   request->input != NULL ? "true" : "false",
					   request->task_name != NULL ? request->task_name : "<anonymous>");
	(void) qx_insert_event(eventrel, request->sessionoid, taskoid, stepoid,
						   request->ownerid, "TASK_INPUT_CAPTURED", payload);
	(void) qx_insert_trace(tracerel, request->sessionoid, taskoid, stepoid,
						   request->ownerid, "runtime.capture_input", payload);
	pfree(payload);

	stepoid = qx_insert_step(steprel, request->sessionoid, taskoid, 5,
							 "stage8.checkpoint_barrier",
							 "Attempt 1 reached a resumable checkpoint barrier");
	qx_charge_task_budget(taskrel, taskoid, 4, 14, "checkpoint_barrier");
	payload = psprintf("checkpoint=stage8.after_capture;task_state=%c",
					   QX_TASK_STATE_CHECKPOINTED);
	(void) qx_insert_event(eventrel, request->sessionoid, taskoid, stepoid,
						   request->ownerid, "TASK_CHECKPOINTED", payload);
	(void) qx_insert_trace(tracerel, request->sessionoid, taskoid, stepoid,
						   request->ownerid, "runtime.checkpoint", payload);

	checkpoint_data = psprintf("task=%u;attempt=%u;session=%u;next_step=%d",
							   taskoid, attemptoid, request->sessionoid, 6);
	checkpointoid = qx_insert_checkpoint(checkpointrel,
										 request->sessionoid,
										 taskoid,
										 attemptoid,
										 stepoid,
										 request->ownerid,
										 QX_TASK_STATE_CHECKPOINTED,
										 6,
										 "stage8.after_capture",
										 checkpoint_data);
	pfree(checkpoint_data);

	qx_update_attempt_state(attemptrel, attemptoid, QX_ATTEMPT_STATE_CHECKPOINTED);
	qx_update_task_runtime(taskrel, taskoid, QX_TASK_STATE_CHECKPOINTED,
						   attemptoid, true, checkpointoid, true);

	(void) qx_insert_event(eventrel, request->sessionoid, taskoid, InvalidOid,
						   request->ownerid, "TASK_READY_FOR_RESUME", payload);
	(void) qx_insert_trace(tracerel, request->sessionoid, taskoid, InvalidOid,
						   request->ownerid, "runtime.pause",
						   "Task paused at durable checkpoint and awaits RESUME TASK");
	pfree(payload);
	qx_free_external_result(&tool_result);

	table_close(checkpointrel, RowExclusiveLock);
	table_close(tracerel, RowExclusiveLock);
	table_close(eventrel, RowExclusiveLock);
	table_close(steprel, RowExclusiveLock);
	table_close(attemptrel, RowExclusiveLock);
	table_close(taskrel, RowExclusiveLock);

	return taskoid;
}

Oid
QxRuntimeResumeTask(Oid taskoid, const char *checkpoint_label, Oid ownerid)
{
	Relation	taskrel;
	Relation	attemptrel;
	Relation	steprel;
	Relation	eventrel;
	Relation	tracerel;
	Relation	checkpointrel;
	HeapTuple	tasktup;
	Form_pg_qx_task taskform;
	HeapTuple	checkpointtup;
	Form_pg_qx_checkpoint checkpointform;
	Oid			attemptoid;
	Oid			stepoid;
	Oid			finalcheckpointoid;
	int16		nextattemptseqno;
	int16		resume_stepseqno;
	List	   *authorized_contracts;
	const char *selected_contract;
	char	   *stored_label;
	char	   *payload;
	char	   *checkpoint_data;
	char	   *goal_text;
	bool		input_present;
	QxExternalToolResult tool_result;

	tasktup = SearchSysCache1(QXTASKOID, ObjectIdGetDatum(taskoid));
	if (!HeapTupleIsValid(tasktup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("task %u does not exist", taskoid)));

	taskform = (Form_pg_qx_task) GETSTRUCT(tasktup);

	if (!has_privs_of_role(ownerid, taskform->qxtaskowner))
	{
		ReleaseSysCache(tasktup);
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to resume task %u", taskoid),
				 errdetail("Only the task owner or a member of that role may resume the task.")));
	}

	if (taskform->qxtaskstate != QX_TASK_STATE_CHECKPOINTED)
	{
		ReleaseSysCache(tasktup);
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("task %u is not resumable", taskoid),
				 errdetail("RESUME TASK only accepts tasks in checkpointed state.")));
	}

	if (!OidIsValid(taskform->qxtasklastcheckpointid))
	{
		ReleaseSysCache(tasktup);
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("task %u has no checkpoint to resume from", taskoid)));
	}

	checkpointtup = SearchSysCache1(QXCHECKPOINTOID,
									ObjectIdGetDatum(taskform->qxtasklastcheckpointid));
	if (!HeapTupleIsValid(checkpointtup))
	{
		ReleaseSysCache(tasktup);
		elog(ERROR, "cache lookup failed for QhapaqXian checkpoint %u",
			 taskform->qxtasklastcheckpointid);
	}

	checkpointform = (Form_pg_qx_checkpoint) GETSTRUCT(checkpointtup);
	stored_label = qx_fetch_checkpoint_label(checkpointtup);

	if (checkpoint_label != NULL &&
		(stored_label == NULL || strcmp(checkpoint_label, stored_label) != 0))
	{
		if (stored_label != NULL)
			pfree(stored_label);
		ReleaseSysCache(checkpointtup);
		ReleaseSysCache(tasktup);
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("checkpoint label \"%s\" does not match the current resumable checkpoint for task %u",
						checkpoint_label, taskoid)));
	}

	nextattemptseqno = 1;
	if (OidIsValid(taskform->qxtasklastattemptid))
		nextattemptseqno = qx_fetch_attempt_seqno(taskform->qxtasklastattemptid) + 1;
	resume_stepseqno = checkpointform->qxcheckpointnextstepseqno;
	if (resume_stepseqno <= 0)
	{
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("checkpoint \"%s\" for task %u is not resumable",
						stored_label != NULL ? stored_label : "<unnamed>", taskoid),
				 errdetail("The checkpoint does not advertise a valid next step sequence.")));
	}

	taskrel = table_open(QxTaskRelationId, RowExclusiveLock);
	attemptrel = table_open(QxAttemptRelationId, RowExclusiveLock);
	steprel = table_open(QxStepRelationId, RowExclusiveLock);
	eventrel = table_open(QxEventRelationId, RowExclusiveLock);
	tracerel = table_open(QxTraceRelationId, RowExclusiveLock);
	checkpointrel = table_open(QxCheckpointRelationId, RowExclusiveLock);

	attemptoid = qx_insert_attempt(attemptrel,
								   taskform->qxtasksessionid,
								   taskoid,
								   ownerid,
								   checkpointform->oid,
								   nextattemptseqno,
								   QX_ATTEMPT_STATE_RUNNING,
								   "resume");
	CommandCounterIncrement();

	qx_update_task_runtime(taskrel, taskoid, QX_TASK_STATE_RUNNING,
						   attemptoid, true, InvalidOid, false);

	authorized_contracts = qx_fetch_task_authorized_contracts(tasktup);
	selected_contract = authorized_contracts != NIL ?
		strVal((Node *) llast(authorized_contracts)) : NULL;
	goal_text = qx_fetch_task_goal(tasktup);
	input_present = qx_task_input_present(tasktup);
	memset(&tool_result, 0, sizeof(tool_result));

	payload = psprintf("checkpoint=%s;resume_requested;next_step=%d",
					   stored_label != NULL ? stored_label : "<unnamed>",
					   checkpointform->qxcheckpointnextstepseqno);
	(void) qx_insert_event(eventrel, taskform->qxtasksessionid, taskoid,
						   InvalidOid, ownerid, "TASK_RESUMED", payload);
	(void) qx_insert_trace(tracerel, taskform->qxtasksessionid, taskoid,
						   InvalidOid, ownerid, "runtime.resume", payload);
	pfree(payload);

	stepoid = qx_insert_step(steprel, taskform->qxtasksessionid, taskoid,
							 resume_stepseqno,
							 "stage8.resume_dispatch",
							 "Attempt 2 resumed execution from the last durable checkpoint");
	qx_charge_task_budget(taskrel, taskoid, 8, 14, "resume_dispatch");
	payload = psprintf("resume_from=%s;state=%c",
					   stored_label != NULL ? stored_label : "<unnamed>",
					   QX_TASK_STATE_RUNNING);
	(void) qx_insert_event(eventrel, taskform->qxtasksessionid, taskoid,
						   stepoid, ownerid, "TASK_RESUME_DISPATCHED", payload);
	(void) qx_insert_trace(tracerel, taskform->qxtasksessionid, taskoid,
						   stepoid, ownerid, "runtime.resume_dispatch", payload);
	pfree(payload);

	stepoid = qx_insert_step(steprel, taskform->qxtasksessionid, taskoid,
							 resume_stepseqno + 1,
							 "stage16.external_resume",
							 "Attempt 2 executed the selected tool through its principal program");
	qx_execute_tool_contract(selected_contract, "resume", taskoid,
							 goal_text, input_present, &tool_result);
	qx_charge_task_budget(taskrel, taskoid,
						  tool_result.token_charge,
						  tool_result.cost_charge,
						  "external_resume");
	payload = psprintf("phase=resume;tool=%s;principal=%s;principal_runtime=%s;provider=%s;provider_kind=%s;effective_sandbox=%s;profile=%s;env=%s;cwd=%s;process_limit=%d;timeout_ms=%d;receipt_schema=%s;receipt_alg=%s;receipt_nonce=%s;receipt_sig=%s;attestation=%s;tokens=%d;cost=%d;detail=%s",
					   tool_result.tool_name != NULL ? tool_result.tool_name : "<unknown>",
					   tool_result.principal_name != NULL ? tool_result.principal_name : "<unknown>",
					   tool_result.principal_runtime != NULL ? tool_result.principal_runtime : "<unknown>",
					   tool_result.provider_name != NULL ? tool_result.provider_name : "<unknown>",
					   tool_result.provider_kind != NULL ? tool_result.provider_kind : "<unknown>",
					   tool_result.sandbox_name != NULL ? tool_result.sandbox_name : "<unknown>",
					   tool_result.profile_name != NULL ? tool_result.profile_name : "<unknown>",
					   tool_result.environment_mode != NULL ? tool_result.environment_mode : "<unknown>",
					   tool_result.workdir_name != NULL ? tool_result.workdir_name : "<unknown>",
					   tool_result.process_limit,
					   tool_result.timeout_ms,
					   tool_result.receipt_schema != NULL ? tool_result.receipt_schema : "<unknown>",
					   tool_result.receipt_alg != NULL ? tool_result.receipt_alg : "<unknown>",
					   tool_result.receipt_nonce != NULL ? tool_result.receipt_nonce : "<unknown>",
					   tool_result.receipt_signature != NULL ? "verified" : "missing",
					   tool_result.attestation_mode != NULL ? tool_result.attestation_mode : "<unknown>",
					   tool_result.token_charge,
					   tool_result.cost_charge,
					   tool_result.detail != NULL ? tool_result.detail : "<none>");
	(void) qx_insert_event(eventrel, taskform->qxtasksessionid, taskoid,
						   stepoid, ownerid, "TASK_TOOL_EXECUTED", payload);
	(void) qx_insert_trace(tracerel, taskform->qxtasksessionid, taskoid,
						   stepoid, ownerid, "runtime.external_resume", payload);
	pfree(payload);

	stepoid = qx_insert_step(steprel, taskform->qxtasksessionid, taskoid,
							 resume_stepseqno + 2,
							 "stage8.complete",
							 "Attempt 2 completed the task after resuming");
	qx_charge_task_budget(taskrel, taskoid, 4, 11, "final_checkpoint");
	payload = psprintf("checkpoint=stage8.final;task_state=%c",
					   QX_TASK_STATE_COMPLETED);
	(void) qx_insert_event(eventrel, taskform->qxtasksessionid, taskoid,
						   stepoid, ownerid, "TASK_CHECKPOINTED", payload);
	(void) qx_insert_trace(tracerel, taskform->qxtasksessionid, taskoid,
						   stepoid, ownerid, "runtime.final_checkpoint", payload);

	checkpoint_data = psprintf("task=%u;attempt=%u;session=%u;next_step=%d",
							   taskoid, attemptoid, taskform->qxtasksessionid, 0);
	finalcheckpointoid = qx_insert_checkpoint(checkpointrel,
											  taskform->qxtasksessionid,
											  taskoid,
											  attemptoid,
											  stepoid,
											  ownerid,
											  QX_TASK_STATE_COMPLETED,
											  0,
											  "stage8.final",
											  checkpoint_data);
	pfree(checkpoint_data);

	qx_update_attempt_state(attemptrel, attemptoid, QX_ATTEMPT_STATE_COMPLETED);
	qx_update_task_runtime(taskrel, taskoid, QX_TASK_STATE_COMPLETED,
						   attemptoid, true, finalcheckpointoid, true);

	(void) qx_insert_event(eventrel, taskform->qxtasksessionid, taskoid,
						   InvalidOid, ownerid, "TASK_COMPLETED", payload);
	(void) qx_insert_trace(tracerel, taskform->qxtasksessionid, taskoid,
						   InvalidOid, ownerid, "runtime.complete",
						   "Task completed after resumable attempt handoff");
	pfree(payload);
	if (goal_text != NULL)
		pfree(goal_text);
	if (authorized_contracts != NIL)
		list_free_deep(authorized_contracts);
	qx_free_external_result(&tool_result);

	if (stored_label != NULL)
		pfree(stored_label);

	ReleaseSysCache(checkpointtup);
	ReleaseSysCache(tasktup);
	table_close(checkpointrel, RowExclusiveLock);
	table_close(tracerel, RowExclusiveLock);
	table_close(eventrel, RowExclusiveLock);
	table_close(steprel, RowExclusiveLock);
	table_close(attemptrel, RowExclusiveLock);
	table_close(taskrel, RowExclusiveLock);

	return taskoid;
}
