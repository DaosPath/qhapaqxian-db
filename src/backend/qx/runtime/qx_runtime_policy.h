/*-------------------------------------------------------------------------
 *
 * qx_runtime_policy.h
 *	  runtime execution policy derived from capability tags
 *
 *-------------------------------------------------------------------------
 */
#ifndef QX_RUNTIME_POLICY_H
#define QX_RUNTIME_POLICY_H

#include "qx/qx_security.h"

typedef struct QxRuntimePolicy
{
	bool		allow_network;
	bool		allow_privilege_escalation;
	char	   *image_ref;
} QxRuntimePolicy;

extern void qx_runtime_policy_init(QxRuntimePolicy *policy);
extern void qx_runtime_policy_free(QxRuntimePolicy *policy);
extern void qx_runtime_policy_from_authz(const QxToolAuthorization *authz,
										 const char *tool_capability_tags,
										 QxRuntimePolicy *policy);
extern void qx_runtime_policy_validate_image_ref(const char *image_ref);

#endif							/* QX_RUNTIME_POLICY_H */