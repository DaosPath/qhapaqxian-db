/*-------------------------------------------------------------------------
 *
 * pg_qx_task.h
 *	  definition of the "agent task" system catalog (pg_qx_task)
 *
 * Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/catalog/pg_qx_task.h
 *
 * NOTES
 *	  The Catalog.pm module reads this file and derives schema
 *	  information.
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_QX_TASK_H
#define PG_QX_TASK_H

#include "catalog/genbki.h"
#include "catalog/pg_qx_task_d.h"

#define QX_TASK_STATE_PENDING 'p'
#define QX_TASK_STATE_QUEUED 'q'
#define QX_TASK_STATE_RUNNING 'r'
#define QX_TASK_STATE_CHECKPOINTED 'k'
#define QX_TASK_STATE_COMPLETED 'c'
#define QX_TASK_STATE_FAILED 'f'

/* ----------------
 *		pg_qx_task definition.  cpp turns this into
 *		typedef struct FormData_pg_qx_task
 * ----------------
 */
CATALOG(pg_qx_task,9532,QxTaskRelationId)
{
	Oid			oid;			/* oid */
	Oid			qxtaskdbid BKI_LOOKUP(pg_database);	/* database */
	Oid			qxtasksessionid BKI_LOOKUP(pg_qx_session);	/* parent session */
	Oid			qxtaskagentid BKI_LOOKUP(pg_qx_agent);	/* denormalized owner */
	Oid			qxtasknamespacepolicyid BKI_LOOKUP(pg_qx_namespace);	/* snapped namespace policy */
	Oid			qxtaskidentityid BKI_LOOKUP(pg_qx_identity);	/* execution identity */
	Oid			qxtaskowner BKI_LOOKUP(pg_authid);	/* request owner */
	Oid			qxtasklastattemptid BKI_DEFAULT(0) BKI_LOOKUP_OPT(pg_qx_attempt);	/* last attempt */
	Oid			qxtasklastcheckpointid BKI_DEFAULT(0) BKI_LOOKUP_OPT(pg_qx_checkpoint);	/* last durable checkpoint */
	int32		qxtaskbudgettokens BKI_DEFAULT(0);	/* snapshot token ceiling */
	int32		qxtaskbudgetcost BKI_DEFAULT(0);	/* snapshot cost ceiling */
	int32		qxtaskauthorizedtooltokens BKI_DEFAULT(0);	/* snapped tool token units */
	int32		qxtaskauthorizedtoolcost BKI_DEFAULT(0);	/* snapped tool cost units */
	int32		qxtaskestimatedtokens BKI_DEFAULT(0);	/* reserved token estimate */
	int32		qxtaskestimatedcost BKI_DEFAULT(0);	/* reserved cost estimate */
	int32		qxtaskconsumedtokens BKI_DEFAULT(0);	/* metered runtime token usage */
	int32		qxtaskconsumedcost BKI_DEFAULT(0);	/* metered runtime cost usage */
	char		qxtaskstate;	/* see QX_TASK_STATE_* */

#ifdef CATALOG_VARLEN
	text		qxtaskname BKI_FORCE_NULL;	/* optional symbolic task name */
	text		qxtaskgoal BKI_FORCE_NULL;	/* goal text */
	pg_node_tree qxtaskinput BKI_FORCE_NULL;	/* serialized raw input expr */
	text		qxtaskpriority BKI_FORCE_NULL;	/* priority label */
	pg_node_tree qxtaskauthorizedtools BKI_FORCE_NULL;	/* serialized List<String> */
	text		qxtasksubmitcontract BKI_FORCE_NULL;	/* explicit submit-phase route */
	text		qxtaskresumecontract BKI_FORCE_NULL;	/* explicit resume-phase route */
#endif
} FormData_pg_qx_task;

typedef FormData_pg_qx_task *Form_pg_qx_task;

DECLARE_TOAST(pg_qx_task, 9534, 9535);

DECLARE_UNIQUE_INDEX_PKEY(pg_qx_task_oid_index, 9536, QxTaskOidIndexId, pg_qx_task, btree(oid oid_ops));
DECLARE_INDEX(pg_qx_task_sessionid_index, 9537, QxTaskSessionIdIndexId, pg_qx_task, btree(qxtasksessionid oid_ops));

MAKE_SYSCACHE(QXTASKOID, pg_qx_task_oid_index, 8);

#endif							/* PG_QX_TASK_H */
