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
	bool		readonly_rootfs;
	char	   *image_ref;
	char	   *seccomp_mode;
	char	   *oci_profile;
	char	   *kernel_ref;
	char	   *initrd_ref;
	char	   *snapshot_ref;
} QxRuntimePolicy;

extern void qx_runtime_policy_init(QxRuntimePolicy *policy);
extern void qx_runtime_policy_free(QxRuntimePolicy *policy);
extern void qx_runtime_policy_from_authz(const QxToolAuthorization *authz,
										 const char *tool_capability_tags,
										 QxRuntimePolicy *policy);
extern void qx_runtime_policy_validate_allowlist_entry(const char *value,
													   const char *env_var,
													   const char *label);
extern void qx_runtime_policy_validate_image_ref(const char *image_ref);
extern void qx_runtime_policy_validate_microvm_assets(const char *kernel,
													  const char *initrd,
													  const char *snapshot);
extern void qx_runtime_policy_compile_container(QxRuntimePolicy *policy);
extern void qx_runtime_policy_resolve_microvm_assets(QxRuntimePolicy *policy,
													 const char *kernel_path,
													 const char *initrd_path);

#endif							/* QX_RUNTIME_POLICY_H */