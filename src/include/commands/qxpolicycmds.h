/*-------------------------------------------------------------------------
 *
 * qxpolicycmds.h
 *	  prototypes for QhapaqXian namespace policy and tool commands
 *
 *-------------------------------------------------------------------------
 */
#ifndef QXPOLICYCMDS_H
#define QXPOLICYCMDS_H

#include "nodes/parsenodes.h"

extern void CreateNamespacePolicyCommand(CreateNamespacePolicyStmt *stmt);
extern void AlterNamespacePolicyCommand(AlterNamespacePolicyStmt *stmt);
extern void CreateProviderCommand(CreateProviderStmt *stmt);
extern void AlterProviderCommand(AlterProviderStmt *stmt);
extern void CreatePrincipalCommand(CreatePrincipalStmt *stmt);
extern void AlterPrincipalCommand(AlterPrincipalStmt *stmt);
extern void CreateToolCommand(CreateToolStmt *stmt);
extern void AlterToolCommand(AlterToolStmt *stmt);

#endif							/* QXPOLICYCMDS_H */
