/*-------------------------------------------------------------------------
 *
 * qx_runtime_policy.c
 *	  runtime execution policy derived from capability tags
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "miscadmin.h"
#include "qx_runtime_policy.h"
#include "utils/memutils.h"

static bool qx_runtime_policy_tag_value(const char *serialized,
										const char *prefix,
										char *dest, size_t destlen);

void
qx_runtime_policy_init(QxRuntimePolicy *policy)
{
	if (policy == NULL)
		return;

	MemSet(policy, 0, sizeof(*policy));
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

void
qx_runtime_policy_from_authz(const QxToolAuthorization *authz,
							 const char *tool_capability_tags,
							 QxRuntimePolicy *policy)
{
	char		network_value[64];
	char		privilege_value[64];
	char		image_value[256];
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
}

void
qx_runtime_policy_validate_image_ref(const char *image_ref)
{
	const char *allowlist_env;
	char	   *copy;
	char	   *cursor;
	bool		allowed = false;

	if (image_ref == NULL || image_ref[0] == '\0')
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("runtime policy requires a container image reference")));

	allowlist_env = getenv("QX_CONTAINER_IMAGE_ALLOWLIST");
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

		if (strcmp(entry, image_ref) == 0)
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
				 errmsg("container image \"%s\" is not in QX_CONTAINER_IMAGE_ALLOWLIST",
						image_ref)));
}