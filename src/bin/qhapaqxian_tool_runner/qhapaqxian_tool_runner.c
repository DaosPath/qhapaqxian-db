/*
 * qhapaqxian_tool_runner.c
 *
 * Deterministic external tool runner for the QhapaqXian runtime bootstrap.
 * It reads a key/value request file and writes metering plus an operator-
 * readable detail line to a response file.
 */

#include "postgres_fe.h"

#include "common/hmac.h"
#include "common/openssl.h"
#include "common/sha2.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <direct.h>
#define getcwd _getcwd
#else
#include <unistd.h>
#endif

#ifdef USE_OPENSSL
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#endif

typedef struct Request
{
	char	phase[32];
	char	tool[128];
	char	handler[256];
	char	sandbox[64];
	char	principal[128];
	char	principal_runtime[64];
	char	provider[128];
	char	provider_kind[64];
	char	provider_endpoint[256];
	char	receipt_schema[64];
	char	receipt_alg[64];
	char	receipt_nonce[256];
	char	receipt_key[256];
	char	receipt_signer[260];
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

#define QX_RECEIPT_SIG_HEX_LEN	((PG_SHA256_DIGEST_LENGTH * 2) + 1)
#define QX_ED25519_SIG_HEX_LEN	((64 * 2) + 1)
#define QX_RECEIPT_SIG_HEX_MAXLEN QX_ED25519_SIG_HEX_LEN

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

static const char *
receipt_attestation_mode(const Request *request)
{
	if (!request->require_attestation)
		return "optional";
	if (strcmp(request->provider_kind, "microvm") == 0)
		return "microvm_receipt_verified";
	if (strcmp(request->provider_kind, "container") == 0)
		return "container_receipt_verified";
	if (strcmp(request->provider_kind, "remote") == 0)
		return "remote_broker_verified";
	return "loopback_verified";
}

static int
encode_signature_hex(const unsigned char *signature, size_t siglen,
					 char *dest, size_t destlen)
{
	size_t		i;

	if (destlen < (siglen * 2) + 1)
		return 0;

	for (i = 0; i < siglen; i++)
		snprintf(dest + (i * 2), 3, "%02x", signature[i]);
	dest[(siglen * 2)] = '\0';
	return 1;
}

static int
compute_hmac_receipt_signature(const char *receipt_key, const char *payload,
							   char *dest, size_t destlen)
{
	pg_hmac_ctx *ctx;
	uint8		digest[PG_SHA256_DIGEST_LENGTH];

	if (receipt_key == NULL || receipt_key[0] == '\0' ||
		destlen < QX_RECEIPT_SIG_HEX_LEN)
		return 0;

	ctx = pg_hmac_create(PG_SHA256);
	if (ctx == NULL)
		return 0;
	if (pg_hmac_init(ctx, (const uint8 *) receipt_key, strlen(receipt_key)) < 0 ||
		pg_hmac_update(ctx, (const uint8 *) payload, strlen(payload)) < 0 ||
		pg_hmac_final(ctx, digest, sizeof(digest)) < 0)
	{
		pg_hmac_free(ctx);
		return 0;
	}
	pg_hmac_free(ctx);

	if (!encode_signature_hex(digest, sizeof(digest), dest, destlen))
	{
		explicit_bzero(digest, sizeof(digest));
		return 0;
	}
	explicit_bzero(digest, sizeof(digest));
	return 1;
}

#ifdef USE_OPENSSL
static void
openssl_error_string(char *dest, size_t destlen)
{
	unsigned long errcode;

	errcode = ERR_get_error();
	if (errcode == 0)
		snprintf(dest, destlen, "%s", "no OpenSSL error reported");
	else
		ERR_error_string_n(errcode, dest, destlen);
}

static int
compute_ed25519_receipt_signature(const char *signer_path, const char *payload,
								  char *dest, size_t destlen)
{
	BIO		   *bio = NULL;
	EVP_PKEY   *pkey = NULL;
	EVP_MD_CTX *mdctx = NULL;
	unsigned char signature[64];
	size_t		siglen = sizeof(signature);
	int			ok = 0;

	if (signer_path == NULL || signer_path[0] == '\0')
		return 0;

	bio = BIO_new_file(signer_path, "r");
	if (bio == NULL)
		return 0;

	pkey = PEM_read_bio_PrivateKey(bio, NULL, NULL, NULL);
	if (pkey == NULL)
	{
		BIO_free(bio);
		return 0;
	}

	mdctx = EVP_MD_CTX_new();
	if (mdctx == NULL)
	{
		EVP_PKEY_free(pkey);
		BIO_free(bio);
		return 0;
	}

	if (EVP_DigestSignInit(mdctx, NULL, NULL, NULL, pkey) != 1)
		goto done;
	if (EVP_DigestSign(mdctx, signature, &siglen,
					   (const unsigned char *) payload,
					   strlen(payload)) != 1)
		goto done;
	ok = encode_signature_hex(signature, siglen, dest, destlen);

done:
	explicit_bzero(signature, sizeof(signature));
	EVP_MD_CTX_free(mdctx);
	EVP_PKEY_free(pkey);
	BIO_free(bio);
	return ok;
}
#endif

static int
compute_receipt_signature(const Request *request, const char *payload,
						  char *dest, size_t destlen)
{
	const char *receipt_alg;

	receipt_alg = request->receipt_alg[0] != '\0' ?
		request->receipt_alg : "hmac-sha256";

	if (strcmp(receipt_alg, "ed25519") == 0)
	{
#ifdef USE_OPENSSL
		return compute_ed25519_receipt_signature(request->receipt_signer,
												 payload,
												 dest,
												 destlen);
#else
		return 0;
#endif
	}

	return compute_hmac_receipt_signature(request->receipt_key,
										  payload,
										  dest,
										  destlen);
}

static void
build_receipt_payload(char *buffer, size_t buflen, const Request *request,
					  const char *environment_mode, const char *workdir_name,
					  const char *attestation_mode, const char *receipt_schema,
					  const char *receipt_alg, int tokens, int cost,
					  const char *detail)
{
	snprintf(buffer, buflen,
			 "phase=%s;task=%ld;tool=%s;principal=%s;principal_runtime=%s;provider=%s;provider_kind=%s;provider_endpoint=%s;sandbox=%s;profile=%s;env=%s;workdir=%s;timeout_ms=%ld;process_limit=%ld;path_present=%s;receipt_schema=%s;receipt_alg=%s;receipt_nonce=%s;attestation=%s;tokens=%d;cost=%d;detail=%s",
			 request->phase[0] != '\0' ? request->phase : "submit",
			 request->task_oid,
			 request->tool[0] != '\0' ? request->tool : "<unknown>",
			 request->principal[0] != '\0' ? request->principal : "<unknown>",
			 request->principal_runtime[0] != '\0' ? request->principal_runtime : "host",
			 request->provider[0] != '\0' ? request->provider : "<unknown>",
			 request->provider_kind[0] != '\0' ? request->provider_kind : "loopback",
			 request->provider_endpoint[0] != '\0' ? request->provider_endpoint : "local://qhapaqxian-tool-runner",
			 request->sandbox[0] != '\0' ? request->sandbox : "builtin",
			 request->profile[0] != '\0' ? request->profile : request->sandbox,
			 environment_mode,
			 workdir_name,
			 request->timeout_ms,
			 request->process_limit,
			 request->path_present ? "true" : "false",
			 receipt_schema,
			 receipt_alg,
			 request->receipt_nonce[0] != '\0' ? request->receipt_nonce : "missing",
			 attestation_mode,
			 tokens,
			 cost,
			 detail);
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
		else if (strcmp(key, "PRINCIPAL_RUNTIME") == 0)
			snprintf(request->principal_runtime, sizeof(request->principal_runtime), "%s", value);
		else if (strcmp(key, "PROVIDER") == 0)
			snprintf(request->provider, sizeof(request->provider), "%s", value);
		else if (strcmp(key, "PROVIDER_KIND") == 0)
			snprintf(request->provider_kind, sizeof(request->provider_kind), "%s", value);
		else if (strcmp(key, "PROVIDER_ENDPOINT") == 0)
			snprintf(request->provider_endpoint, sizeof(request->provider_endpoint), "%s", value);
		else if (strcmp(key, "RECEIPT_SCHEMA") == 0)
			snprintf(request->receipt_schema, sizeof(request->receipt_schema), "%s", value);
		else if (strcmp(key, "RECEIPT_ALG") == 0)
			snprintf(request->receipt_alg, sizeof(request->receipt_alg), "%s", value);
		else if (strcmp(key, "RECEIPT_NONCE") == 0)
			snprintf(request->receipt_nonce, sizeof(request->receipt_nonce), "%s", value);
		else if (strcmp(key, "RECEIPT_KEY") == 0)
			snprintf(request->receipt_key, sizeof(request->receipt_key), "%s", value);
		else if (strcmp(key, "RECEIPT_SIGNER") == 0)
			snprintf(request->receipt_signer, sizeof(request->receipt_signer), "%s", value);
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
	const char *environment_mode;
	const char *workdir_name;
	const char *receipt_schema;
	const char *receipt_alg;
	const char *attestation_mode;
	char		detail[512];
	char		payload[4096];
	char		receipt_sig[QX_RECEIPT_SIG_HEX_MAXLEN];
#ifdef USE_OPENSSL
	char		openssl_error[256];
#endif

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
	environment_mode = request.path_present ? "ambient" : "minimal";
	workdir_name = path_basename(request.workdir);
	receipt_schema = request.receipt_schema[0] != '\0' ? request.receipt_schema : "qx.receipt.v1";
	receipt_alg = request.receipt_alg[0] != '\0' ? request.receipt_alg : "hmac-sha256";
	attestation_mode = receipt_attestation_mode(&request);
	snprintf(detail, sizeof(detail), "tool %s via %s for %s phase on task %ld",
			 request.tool,
			 request.principal,
			 request.phase[0] != '\0' ? request.phase : "submit",
			 request.task_oid);
	build_receipt_payload(payload, sizeof(payload), &request, environment_mode,
						  workdir_name, attestation_mode, receipt_schema,
						  receipt_alg, tokens, cost, detail);
	if (!compute_receipt_signature(&request, payload,
								   receipt_sig, sizeof(receipt_sig)))
	{
#ifdef USE_OPENSSL
		if (strcmp(receipt_alg, "ed25519") == 0)
		{
			openssl_error_string(openssl_error, sizeof(openssl_error));
			fprintf(stderr, "could not sign receipt payload with ed25519 signer: %s\n",
					openssl_error);
		}
		else
#endif
		fprintf(stderr, "could not sign receipt payload\n");
		return 1;
	}

	response = fopen(response_path, "w");
	if (response == NULL)
	{
		fprintf(stderr, "could not open response file\n");
		return 1;
	}

	fprintf(response, "STATUS=ok\n");
	fprintf(response, "TOOL=%s\n", request.tool);
	fprintf(response, "PRINCIPAL=%s\n", request.principal);
	fprintf(response, "PRINCIPAL_RUNTIME=%s\n", request.principal_runtime[0] != '\0' ? request.principal_runtime : "host");
	fprintf(response, "PROVIDER=%s\n", request.provider);
	fprintf(response, "PROVIDER_KIND=%s\n", request.provider_kind[0] != '\0' ? request.provider_kind : "loopback");
	fprintf(response, "SANDBOX=%s\n", request.sandbox);
	fprintf(response, "PROFILE=%s\n", request.profile[0] != '\0' ? request.profile : request.sandbox);
	fprintf(response, "ENV=%s\n", environment_mode);
	fprintf(response, "WORKDIR=%s\n", workdir_name);
	fprintf(response, "TIMEOUT_MS=%ld\n", request.timeout_ms);
	fprintf(response, "PROCESS_LIMIT=%ld\n", request.process_limit);
	fprintf(response, "PATH_PRESENT=%s\n", request.path_present ? "true" : "false");
	fprintf(response, "RECEIPT_SCHEMA=%s\n", receipt_schema);
	fprintf(response, "RECEIPT_ALG=%s\n", receipt_alg);
	fprintf(response, "RECEIPT_NONCE=%s\n", request.receipt_nonce[0] != '\0' ? request.receipt_nonce : "missing");
	fprintf(response, "RECEIPT_SIG=%s\n", receipt_sig);
	fprintf(response, "ATTESTATION=%s\n", attestation_mode);
	fprintf(response, "TOKENS=%d\n", tokens);
	fprintf(response, "COST=%d\n", cost);
	fprintf(response, "DETAIL=%s\n", detail);

	fclose(response);
	return 0;
}
