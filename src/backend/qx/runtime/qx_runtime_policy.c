/*-------------------------------------------------------------------------
 *
 * qx_runtime_policy.c
 *	  runtime execution policy derived from capability tags
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "qx_runtime_policy.h"
#include "utils/memutils.h"

static bool qx_runtime_policy_tag_value(const char *serialized,
										const char *prefix,
										char *dest, size_t destlen);
static bool qx_runtime_policy_parse_bool_tag(const char *value, bool default_value);
static bool qx_runtime_policy_mode_is(const char *value, const char *expected);

void
qx_runtime_policy_init(QxRuntimePolicy *policy)
{
	if (policy == NULL)
		return;

	MemSet(policy, 0, sizeof(*policy));
	policy->readonly_rootfs = true;
}

void
qx_runtime_policy_free(QxRuntimePolicy *policy)
{
	if (policy == NULL)
		return;

	if (policy->image_ref != NULL)
	{
		pfree(policy->image_ref);
		policy->image_ref = NULL;
	}
	if (policy->seccomp_mode != NULL)
	{
		pfree(policy->seccomp_mode);
		policy->seccomp_mode = NULL;
	}
	if (policy->cgroup_mode != NULL)
	{
		pfree(policy->cgroup_mode);
		policy->cgroup_mode = NULL;
	}
	if (policy->oci_profile != NULL)
	{
		pfree(policy->oci_profile);
		policy->oci_profile = NULL;
	}
	if (policy->kernel_ref != NULL)
	{
		pfree(policy->kernel_ref);
		policy->kernel_ref = NULL;
	}
	if (policy->initrd_ref != NULL)
	{
		pfree(policy->initrd_ref);
		policy->initrd_ref = NULL;
	}
	if (policy->snapshot_ref != NULL)
	{
		pfree(policy->snapshot_ref);
		policy->snapshot_ref = NULL;
	}
}

static bool
qx_runtime_policy_tag_value(const char *serialized, const char *prefix,
							char *dest, size_t destlen)
{
	const char *scan;
	size_t		prefixlen;

	if (serialized == NULL || prefix == NULL || dest == NULL || destlen == 0)
		return false;

	prefixlen = strlen(prefix);
	scan = serialized;
	while ((scan = strstr(scan, prefix)) != NULL)
	{
		const char *value;
		const char *end;
		size_t		valuelen;

		if (scan != serialized && scan[-1] != '|')
		{
			scan += prefixlen;
			continue;
		}

		value = scan + prefixlen;
		end = strchr(value, '|');
		valuelen = end != NULL ? (size_t) (end - value) : strlen(value);
		if (valuelen >= destlen)
			valuelen = destlen - 1;
		memcpy(dest, value, valuelen);
		dest[valuelen] = '\0';
		return true;
	}

	return false;
}

static bool
qx_runtime_policy_parse_bool_tag(const char *value, bool default_value)
{
	if (value == NULL || value[0] == '\0')
		return default_value;

	if (pg_strcasecmp(value, "allow") == 0 ||
		pg_strcasecmp(value, "true") == 0 ||
		pg_strcasecmp(value, "yes") == 0 ||
		pg_strcasecmp(value, "1") == 0)
		return true;

	if (pg_strcasecmp(value, "deny") == 0 ||
		pg_strcasecmp(value, "false") == 0 ||
		pg_strcasecmp(value, "no") == 0 ||
		pg_strcasecmp(value, "0") == 0)
		return false;

	return default_value;
}

static bool
qx_runtime_policy_mode_is(const char *value, const char *expected)
{
	return value != NULL && expected != NULL &&
		pg_strcasecmp(value, expected) == 0;
}

void
qx_runtime_policy_from_authz(const QxToolAuthorization *authz,
							 const char *tool_capability_tags,
							 QxRuntimePolicy *policy)
{
	char		network_value[64];
	char		privilege_value[64];
	char		image_value[256];
	char		readonly_value[64];
	char		seccomp_value[64];
	char		cgroup_value[64];
	char		kernel_value[256];
	char		initrd_value[256];
	char		snapshot_value[256];
	const char *tags;

	(void) authz;

	if (policy == NULL)
		return;

	qx_runtime_policy_init(policy);
	tags = tool_capability_tags;

	if (qx_runtime_policy_tag_value(tags, "network:", network_value,
									sizeof(network_value)))
	{
		policy->allow_network =
			(pg_strcasecmp(network_value, "allow") == 0 ||
			 pg_strcasecmp(network_value, "true") == 0 ||
			 pg_strcasecmp(network_value, "yes") == 0);
	}

	if (qx_runtime_policy_tag_value(tags, "privilege_escalation:", privilege_value,
									sizeof(privilege_value)))
	{
		policy->allow_privilege_escalation =
			(pg_strcasecmp(privilege_value, "allow") == 0 ||
			 pg_strcasecmp(privilege_value, "true") == 0 ||
			 pg_strcasecmp(privilege_value, "yes") == 0);
	}

	if (qx_runtime_policy_tag_value(tags, "readonly_rootfs:", readonly_value,
									sizeof(readonly_value)))
		policy->readonly_rootfs =
			qx_runtime_policy_parse_bool_tag(readonly_value, true);

	if (qx_runtime_policy_tag_value(tags, "seccomp_mode:", seccomp_value,
									sizeof(seccomp_value)) &&
		seccomp_value[0] != '\0')
		policy->seccomp_mode = pstrdup(seccomp_value);
	else if (qx_runtime_policy_tag_value(tags, "seccomp:", seccomp_value,
										 sizeof(seccomp_value)) &&
			 seccomp_value[0] != '\0')
		policy->seccomp_mode = pstrdup(seccomp_value);

	if (qx_runtime_policy_tag_value(tags, "cgroup_mode:", cgroup_value,
									sizeof(cgroup_value)) &&
		cgroup_value[0] != '\0')
		policy->cgroup_mode = pstrdup(cgroup_value);
	else if (qx_runtime_policy_tag_value(tags, "cgroup:", cgroup_value,
										 sizeof(cgroup_value)) &&
			 cgroup_value[0] != '\0')
		policy->cgroup_mode = pstrdup(cgroup_value);

	if (qx_runtime_policy_tag_value(tags, "image:", image_value,
									sizeof(image_value)) &&
		image_value[0] != '\0')
		policy->image_ref = pstrdup(image_value);
	else
	{
		const char *default_image = getenv("QX_CONTAINER_IMAGE");

		if (default_image != NULL && default_image[0] != '\0')
			policy->image_ref = pstrdup(default_image);
		else
			policy->image_ref = pstrdup("alpine:3.20");
	}

	if (qx_runtime_policy_tag_value(tags, "kernel:", kernel_value,
									sizeof(kernel_value)) &&
		kernel_value[0] != '\0')
		policy->kernel_ref = pstrdup(kernel_value);

	if (qx_runtime_policy_tag_value(tags, "initrd:", initrd_value,
									sizeof(initrd_value)) &&
		initrd_value[0] != '\0')
		policy->initrd_ref = pstrdup(initrd_value);

	if (qx_runtime_policy_tag_value(tags, "snapshot:", snapshot_value,
									sizeof(snapshot_value)) &&
		snapshot_value[0] != '\0')
		policy->snapshot_ref = pstrdup(snapshot_value);
}

void
qx_runtime_policy_validate_allowlist_entry(const char *value,
										   const char *env_var,
										   const char *label)
{
	const char *allowlist_env;
	char	   *copy;
	char	   *cursor;
	bool		allowed = false;

	if (value == NULL || value[0] == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("runtime policy requires a %s reference", label)));

	allowlist_env = env_var != NULL ? getenv(env_var) : NULL;
	if (allowlist_env == NULL || allowlist_env[0] == '\0')
		return;

	copy = pstrdup(allowlist_env);
	cursor = copy;
	while (cursor != NULL && *cursor != '\0')
	{
		char	   *comma = strchr(cursor, ',');
		char	   *entry;

		if (comma != NULL)
			*comma = '\0';

		entry = cursor;
		while (*entry != '\0' && isspace((unsigned char) *entry))
			entry++;

		if (strcmp(entry, value) == 0)
		{
			allowed = true;
			break;
		}

		cursor = comma != NULL ? comma + 1 : NULL;
	}

	pfree(copy);

	if (!allowed)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("%s \"%s\" is not in %s",
						label, value, env_var)));
}

void
qx_runtime_policy_validate_image_ref(const char *image_ref)
{
	qx_runtime_policy_validate_allowlist_entry(image_ref,
											   "QX_CONTAINER_IMAGE_ALLOWLIST",
											   "container image");
}

void
qx_runtime_policy_validate_microvm_assets(const char *kernel,
										  const char *initrd,
										  const char *snapshot)
{
	qx_runtime_policy_validate_allowlist_entry(kernel,
											   "QX_MICROVM_KERNEL_ALLOWLIST",
											   "microVM kernel");
	qx_runtime_policy_validate_allowlist_entry(initrd,
											   "QX_MICROVM_INITRD_ALLOWLIST",
											   "microVM initrd");

	if (snapshot != NULL && snapshot[0] != '\0')
		qx_runtime_policy_validate_allowlist_entry(snapshot,
												   "QX_MICROVM_SNAPSHOT_ALLOWLIST",
												   "microVM snapshot");
}

void
qx_runtime_policy_compile_container(QxRuntimePolicy *policy)
{
	StringInfoData profile;

	if (policy == NULL)
		return;

	if (policy->seccomp_mode == NULL || policy->seccomp_mode[0] == '\0')
	{
		if (policy->allow_privilege_escalation)
			policy->seccomp_mode = pstrdup("unconfined");
		else
			policy->seccomp_mode = pstrdup("no-new-privileges");
	}
	else if (!qx_runtime_policy_mode_is(policy->seccomp_mode, "unconfined") &&
			 !qx_runtime_policy_mode_is(policy->seccomp_mode, "no-new-privileges") &&
			 !qx_runtime_policy_mode_is(policy->seccomp_mode, "runtime-default") &&
			 !qx_runtime_policy_mode_is(policy->seccomp_mode, "strict"))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("unsupported container seccomp mode \"%s\"",
						policy->seccomp_mode)));

	if (policy->cgroup_mode == NULL || policy->cgroup_mode[0] == '\0')
		policy->cgroup_mode = pstrdup("private");
	else if (!qx_runtime_policy_mode_is(policy->cgroup_mode, "host") &&
			 !qx_runtime_policy_mode_is(policy->cgroup_mode, "private") &&
			 !qx_runtime_policy_mode_is(policy->cgroup_mode, "isolated"))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("unsupported container cgroup mode \"%s\"",
						policy->cgroup_mode)));

	if (policy->oci_profile != NULL)
	{
		pfree(policy->oci_profile);
		policy->oci_profile = NULL;
	}

	initStringInfo(&profile);
	appendStringInfo(&profile,
					 "network=%s;privilege_escalation=%s;readonly_rootfs=%s;seccomp=%s;cgroup=%s;image=%s",
					 policy->allow_network ? "allow" : "deny",
					 policy->allow_privilege_escalation ? "allow" : "deny",
					 policy->readonly_rootfs ? "true" : "false",
					 policy->seccomp_mode,
					 policy->cgroup_mode,
					 policy->image_ref != NULL ? policy->image_ref : "");
	policy->oci_profile = profile.data;
}

void
qx_runtime_policy_resolve_microvm_assets(QxRuntimePolicy *policy,
										 const char *kernel_path,
										 const char *initrd_path)
{
	const char *kernel_default;
	const char *initrd_default;

	if (policy == NULL)
		return;

	kernel_default = getenv("QX_MICROVM_KERNEL");
	initrd_default = getenv("QX_MICROVM_INITRD");

	if (policy->kernel_ref == NULL || policy->kernel_ref[0] == '\0')
	{
		if (kernel_path != NULL && kernel_path[0] != '\0')
			policy->kernel_ref = pstrdup(kernel_path);
		else if (kernel_default != NULL && kernel_default[0] != '\0')
			policy->kernel_ref = pstrdup(kernel_default);
	}

	if (policy->initrd_ref == NULL || policy->initrd_ref[0] == '\0')
	{
		if (initrd_path != NULL && initrd_path[0] != '\0')
			policy->initrd_ref = pstrdup(initrd_path);
		else if (initrd_default != NULL && initrd_default[0] != '\0')
			policy->initrd_ref = pstrdup(initrd_default);
	}

	if (policy->kernel_ref == NULL || policy->kernel_ref[0] == '\0' ||
		policy->initrd_ref == NULL || policy->initrd_ref[0] == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("runtime policy requires microVM kernel and initrd references")));

	qx_runtime_policy_validate_microvm_assets(policy->kernel_ref,
											  policy->initrd_ref,
											  policy->snapshot_ref);
}
