/*-------------------------------------------------------------------------
 *
 * qx_agent_planner.c
 *	  Stage 8 metadata-driven agent planner for QhapaqXian Engine
 *
 * Copyright (c) 1996-2024, PostgreSQL Global Development Group
 *
 * src/backend/qx/planner/qx_agent_planner.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/tupdesc.h"
#include "catalog/pg_qx_agent.h"
#include "catalog/pg_qx_checkpoint.h"
#include "catalog/pg_qx_identity.h"
#include "catalog/pg_qx_namespace.h"
#include "catalog/pg_qx_session.h"
#include "catalog/pg_qx_task.h"
#include "commands/defrem.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/value.h"
#include "qx/qx_agent_planner.h"
#include "qx/qx_security.h"
#include "tcop/tcopprot.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/syscache.h"

static Oid qx_extract_oid_literal(Node *expr, const char *subject,
								  const char *detail, const char *hint);
static QxAgentPlan *qx_make_agent_plan(QxAgentPlanKind kind, Oid ownerid,
									   const char *command_name);
static void qx_agent_plan_add_step(QxAgentPlan *plan, int16 seqno,
								   const char *name, const char *detail,
								   int32 estimated_cost,
								   bool checkpoint_boundary, bool retryable);
static QxAgentPlan *qx_plan_run_task(RunTaskStmt *stmt, Oid ownerid);
static QxAgentPlan *qx_plan_resume_task(ResumeTaskStmt *stmt, Oid ownerid);
static char *qx_fetch_checkpoint_label(HeapTuple checkpointtup);
static void qx_append_plan_text(StringInfo buf, const QxAgentPlan *plan);
static char *qx_text_attr_from_syscache(HeapTuple tup, AttrNumber attnum,
										int cacheid);
static int32 qx_estimate_token_usage(bool input_present, int32 tool_token_cost);
static char *qx_plan_display_contract(const char *contract);

static Oid
qx_extract_oid_literal(Node *expr, const char *subject, const char *detail,
					   const char *hint)
{
	A_Const    *con;
	int32		value;

	if (!IsA(expr, A_Const))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("%s currently requires a literal identifier", subject),
				 errdetail("%s", detail),
				 errhint("%s", hint)));

	con = castNode(A_Const, expr);
	if (nodeTag(&con->val) != T_Integer)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("%s identifier must be an integer literal", subject)));

	value = intVal(&con->val);
	if (value <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("%s identifier must be greater than zero", subject)));

	return (Oid) value;
}

static QxAgentPlan *
qx_make_agent_plan(QxAgentPlanKind kind, Oid ownerid, const char *command_name)
{
	QxAgentPlan *plan;

	plan = palloc0(sizeof(QxAgentPlan));
	plan->kind = kind;
	plan->ownerid = ownerid;
	plan->command_name = pstrdup(command_name);
	plan->estimated_tool_calls = 0;

	return plan;
}

static void
qx_agent_plan_add_step(QxAgentPlan *plan, int16 seqno, const char *name,
					   const char *detail, int32 estimated_cost,
					   bool checkpoint_boundary, bool retryable)
{
	QxAgentPlanStep *step;

	step = palloc0(sizeof(QxAgentPlanStep));
	step->seqno = seqno;
	step->name = pstrdup(name);
	step->detail = pstrdup(detail);
	step->estimated_cost = estimated_cost;
	step->checkpoint_boundary = checkpoint_boundary;
	step->retryable = retryable;

	plan->steps = lappend(plan->steps, step);
	plan->estimated_total_cost += estimated_cost;
}

static char *
qx_plan_display_contract(const char *contract)
{
	char	   *display;
	const char *needle;

	if (contract == NULL)
		return pstrdup("<null>");

	display = pstrdup(contract);

	for (;;)
	{
		const char *keys[] = {"provider_oid=", "receipt_signer="};
		int			i;
		bool		removed = false;

		for (i = 0; i < lengthof(keys); i++)
		{
			char	   *start;
			char	   *end;

			needle = strstr(display, keys[i]);
			if (needle == NULL)
				continue;

			start = unconstify(char *, needle);
			if (start > display && *(start - 1) == ';')
				start--;
			end = strchr(needle, ';');
			if (end == NULL)
				*start = '\0';
			else
				memmove(start, end, strlen(end) + 1);
			removed = true;
			break;
		}

		if (!removed)
			break;
	}

	return display;
}

static QxAgentPlan *
qx_plan_run_task(RunTaskStmt *stmt, Oid ownerid)
{
	Oid			sessionoid;
	HeapTuple	sessiontup;
	Form_pg_qx_session sessionform;
	HeapTuple	agenttup;
	Form_pg_qx_agent agentform;
	HeapTuple	identitytup;
	Form_pg_qx_identity identityform;
	QxAgentPlan *plan;
	char	   *serialized_budget = NULL;
	char	   *serialized_tools = NULL;
	QxBudgetPolicy budget;
	List	   *authorized_tools;
	QxToolAuthorization toolauth;

	sessionoid = qx_extract_oid_literal(
		stmt->session_id,
		"RUN TASK",
		"Etapa 8 routes RUN TASK through AgentPlan planning, but IN SESSION still does not evaluate arbitrary expressions.",
		"Pass a numeric session OID literal, or resolve the session OID in the client before invoking RUN TASK.");

	sessiontup = SearchSysCache1(QXSESSIONOID, ObjectIdGetDatum(sessionoid));
	if (!HeapTupleIsValid(sessiontup))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("session %u does not exist", sessionoid)));

	sessionform = (Form_pg_qx_session) GETSTRUCT(sessiontup);
	agenttup = SearchSysCache1(QXAGENTOID,
							   ObjectIdGetDatum(sessionform->qxsessionagentid));
	if (!HeapTupleIsValid(agenttup))
	{
		ReleaseSysCache(sessiontup);
		elog(ERROR, "cache lookup failed for QhapaqXian agent %u",
			 sessionform->qxsessionagentid);
	}

	agentform = (Form_pg_qx_agent) GETSTRUCT(agenttup);
	identitytup = SearchSysCache1(QXIDENTITYOID,
								  ObjectIdGetDatum(sessionform->qxsessionidentityid));
	if (!HeapTupleIsValid(identitytup))
	{
		ReleaseSysCache(agenttup);
		ReleaseSysCache(sessiontup);
		elog(ERROR, "cache lookup failed for QhapaqXian identity %u",
			 sessionform->qxsessionidentityid);
	}

	identityform = (Form_pg_qx_identity) GETSTRUCT(identitytup);

	if (sessionform->qxsessiondbid != MyDatabaseId)
	{
		ReleaseSysCache(sessiontup);
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("session %u belongs to a different database", sessionoid)));
	}

	if (sessionform->qxsessionstatus != QX_SESSION_STATUS_ACTIVE)
	{
		ReleaseSysCache(sessiontup);
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("session %u is not active", sessionoid),
				 errdetail("RUN TASK in Etapa 8 only accepts sessions in active state.")));
	}

	if (!has_privs_of_role(ownerid, sessionform->qxsessionowner))
	{
		ReleaseSysCache(identitytup);
		ReleaseSysCache(agenttup);
		ReleaseSysCache(sessiontup);
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to run a task in session %u",
						sessionoid),
				 errdetail("Only the session owner or a member of that role may enqueue tasks in the session.")));
	}

	serialized_budget = qx_text_attr_from_syscache(agenttup,
												   Anum_pg_qx_agent_qxbudget,
												   QXAGENTOID);
	serialized_tools = qx_text_attr_from_syscache(agenttup,
												  Anum_pg_qx_agent_qxtools,
												  QXAGENTOID);
	QxBudgetPolicyFromSerialized(serialized_budget, &budget);
	authorized_tools = QxDeserializeToolList(serialized_tools);
	QxValidateToolList(authorized_tools);

	plan = qx_make_agent_plan(QX_AGENT_PLAN_RUN_TASK, ownerid, "RUN TASK");
	plan->sessionoid = sessionoid;
	plan->agentoid = sessionform->qxsessionagentid;
	plan->identityoid = sessionform->qxsessionidentityid;
	plan->namespace_policy_oid = sessionform->qxsessionnamespacepolicyid;
	plan->identity_name = pstrdup(NameStr(identityform->qxidentityname));
	plan->input = stmt->input;
	plan->task_name = stmt->task_name ? pstrdup(stmt->task_name) : NULL;
	plan->goal = pstrdup(stmt->goal);
	plan->priority = stmt->priority ? pstrdup(stmt->priority) : pstrdup("normal");
	plan->checkpoint_label = pstrdup("stage8.after_capture");
	plan->target_state = pstrdup("checkpointed");
	plan->result_mode = stmt->returning ?
		pstrdup("returning requested but still deferred") :
		pstrdup("catalog inspection via pg_qx_*");
	plan->input_present = (stmt->input != NULL);
	plan->checkpointable = true;
	plan->resumable = false;
	plan->returning_requested = stmt->returning;
	QxAuthorizeToolsForNamespace(sessionform->qxsessionnamespacepolicyid,
								 agentform->qxagentnamespace,
								 ownerid,
								 authorized_tools,
								 &toolauth);
	plan->namespace_policy_name = toolauth.namespace_policy_name;
	plan->authorized_tools = toolauth.tool_contracts;
	plan->authorized_tool_oids = toolauth.tool_oids;
	plan->budget_tokens = budget.token_limit;
	plan->budget_cost = budget.cost_limit;
	plan->estimated_tool_calls = toolauth.tool_count;
	plan->authorized_tool_tokens = toolauth.tool_token_cost;
	plan->authorized_tool_cost = toolauth.tool_cost_units;
	plan->estimated_tokens = qx_estimate_token_usage(stmt->input != NULL,
													 toolauth.tool_token_cost);

	qx_agent_plan_add_step(plan, 1,
						   "validate_session",
						   "check session existence, ownership, database, and active state",
						   8, false, false);
	qx_agent_plan_add_step(plan, 2,
						   "snapshot_identity_policy",
						   "freeze agent identity and policy/budget context for the attempt",
						   11, false, false);
	qx_agent_plan_add_step(plan, 3,
						   "authorize_tools",
						   "validate the task tool allowlist against namespace policy and registered tool catalog entries",
						   Max(toolauth.tool_cost_units, 8), false, false);
	qx_agent_plan_add_step(plan, 4,
						   "enqueue_attempt",
						   "persist the task shell and open attempt 1 in the embedded runtime",
						   17, false, true);
	qx_agent_plan_add_step(plan, 5,
						   "capture_input",
						   "record task goal, priority, and raw input for deterministic resume",
						   13, false, false);
	qx_agent_plan_add_step(plan, 6,
						   "durable_checkpoint",
						   "stop at a resumable checkpoint boundary before asynchronous continuation",
						   14, true, true);

	if (toolauth.enforce_budgets &&
		budget.has_cost_limit &&
		plan->estimated_total_cost > budget.cost_limit)
	{
		if (serialized_budget != NULL)
			pfree(serialized_budget);
		if (serialized_tools != NULL)
			pfree(serialized_tools);
		ReleaseSysCache(identitytup);
		ReleaseSysCache(agenttup);
		ReleaseSysCache(sessiontup);
		ereport(ERROR,
				(errcode(ERRCODE_CONFIGURATION_LIMIT_EXCEEDED),
				 errmsg("task plan exceeds the configured cost budget for agent \"%s\"",
						NameStr(agentform->qxagentname)),
				 errdetail("Estimated cost %d exceeds budget limit %d under identity \"%s\".",
						   plan->estimated_total_cost, budget.cost_limit,
						   NameStr(identityform->qxidentityname))));
	}

	if (toolauth.enforce_budgets &&
		budget.has_token_limit &&
		plan->estimated_tokens > budget.token_limit)
	{
		if (serialized_budget != NULL)
			pfree(serialized_budget);
		if (serialized_tools != NULL)
			pfree(serialized_tools);
		ReleaseSysCache(identitytup);
		ReleaseSysCache(agenttup);
		ReleaseSysCache(sessiontup);
		ereport(ERROR,
				(errcode(ERRCODE_CONFIGURATION_LIMIT_EXCEEDED),
				 errmsg("task plan exceeds the configured token budget for agent \"%s\"",
						NameStr(agentform->qxagentname)),
				 errdetail("Estimated token usage %d exceeds budget limit %d under identity \"%s\".",
						   plan->estimated_tokens, budget.token_limit,
						   NameStr(identityform->qxidentityname))));
	}

	if (serialized_budget != NULL)
		pfree(serialized_budget);
	if (serialized_tools != NULL)
		pfree(serialized_tools);
	ReleaseSysCache(identitytup);
	ReleaseSysCache(agenttup);
	ReleaseSysCache(sessiontup);

	return plan;
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

static QxAgentPlan *
qx_plan_resume_task(ResumeTaskStmt *stmt, Oid ownerid)
{
	Oid			taskoid;
	HeapTuple	tasktup;
	Form_pg_qx_task taskform;
	HeapTuple	checkpointtup;
	char	   *stored_label;
	char	   *serialized_tools = NULL;
	QxAgentPlan *plan;

	taskoid = qx_extract_oid_literal(
		stmt->task_id,
		"RESUME TASK",
		"Etapa 8 keeps RESUME TASK on the utility/planner path and does not yet evaluate arbitrary expressions in TASK identifiers.",
		"Pass a numeric task OID literal, or resolve the task OID in the client before invoking RESUME TASK.");

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

	stored_label = qx_fetch_checkpoint_label(checkpointtup);

	if (stmt->checkpoint_label != NULL &&
		(stored_label == NULL || strcmp(stmt->checkpoint_label, stored_label) != 0))
	{
		if (stored_label != NULL)
			pfree(stored_label);
		ReleaseSysCache(checkpointtup);
		ReleaseSysCache(tasktup);
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("checkpoint label \"%s\" does not match the current resumable checkpoint for task %u",
						stmt->checkpoint_label, taskoid)));
	}

	plan = qx_make_agent_plan(QX_AGENT_PLAN_RESUME_TASK, ownerid, "RESUME TASK");
	plan->taskoid = taskoid;
	plan->sessionoid = taskform->qxtasksessionid;
	plan->agentoid = taskform->qxtaskagentid;
	plan->namespace_policy_oid = taskform->qxtasknamespacepolicyid;
	plan->identityoid = taskform->qxtaskidentityid;
	plan->checkpointoid = taskform->qxtasklastcheckpointid;
	plan->identity_name = QxIdentityNameById(taskform->qxtaskidentityid);
	plan->namespace_policy_name = QxNamespacePolicyNameById(taskform->qxtasknamespacepolicyid);
	plan->task_name = NULL;
	plan->goal = NULL;
	plan->priority = NULL;
	plan->checkpoint_label = pstrdup(stored_label != NULL ? stored_label : "<unnamed>");
	plan->target_state = pstrdup("completed");
	plan->result_mode = pstrdup("state transition via pg_qx_task");
	plan->input_present = false;
	plan->checkpointable = true;
	plan->resumable = true;
	plan->budget_tokens = taskform->qxtaskbudgettokens;
	plan->budget_cost = taskform->qxtaskbudgetcost;
	plan->estimated_tokens = taskform->qxtaskestimatedtokens;
	plan->estimated_total_cost = taskform->qxtaskestimatedcost;
	serialized_tools = qx_text_attr_from_syscache(tasktup,
												  Anum_pg_qx_task_qxtaskauthorizedtools,
												  QXTASKOID);
	plan->authorized_tools = QxDeserializeToolList(serialized_tools);
	plan->estimated_tool_calls = list_length(plan->authorized_tools);
	plan->authorized_tool_tokens = taskform->qxtaskauthorizedtooltokens;
	plan->authorized_tool_cost = taskform->qxtaskauthorizedtoolcost;

	qx_agent_plan_add_step(plan, 1,
						   "validate_task",
						   "check task visibility, ownership, and checkpointed state",
						   7, false, false);
	qx_agent_plan_add_step(plan, 2,
						   "load_checkpoint",
						   "load the latest durable checkpoint and verify the requested label",
						   10, true, false);
	qx_agent_plan_add_step(plan, 3,
						   "open_resume_attempt",
						   "create a new runtime attempt linked to the resumable checkpoint",
						   12, false, true);
	qx_agent_plan_add_step(plan, 4,
						   "resume_dispatch",
						   "dispatch the resumed attempt from the stored next-step boundary",
						   14, false, true);
	qx_agent_plan_add_step(plan, 5,
						   "final_checkpoint",
						   "seal completion with a final durable checkpoint and completed task state",
						   11, true, false);

	if (stored_label != NULL)
		pfree(stored_label);
	if (serialized_tools != NULL)
		pfree(serialized_tools);
	ReleaseSysCache(checkpointtup);
	ReleaseSysCache(tasktup);

	return plan;
}

QxAgentPlan *
QxBuildAgentPlan(Node *stmt, Oid ownerid)
{
	switch (nodeTag(stmt))
	{
		case T_RunTaskStmt:
			return qx_plan_run_task((RunTaskStmt *) stmt, ownerid);

		case T_ResumeTaskStmt:
			return qx_plan_resume_task((ResumeTaskStmt *) stmt, ownerid);

		default:
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("statement is not explainable as an agent command yet"),
					 errdetail("Etapa 8 currently exposes AgentPlan only for RUN TASK and RESUME TASK."),
					 errhint("Use EXPLAIN AGENT with RUN TASK or RESUME TASK while deeper agent planning is still landing.")));
	}
}

TupleDesc
QxExplainAgentResultDesc(void)
{
	TupleDesc	tupdesc;

	tupdesc = CreateTemplateTupleDesc(1);
	TupleDescInitEntry(tupdesc, (AttrNumber) 1, "AGENT PLAN",
					   TEXTOID, -1, 0);

	return tupdesc;
}

static void
qx_append_plan_text(StringInfo buf, const QxAgentPlan *plan)
{
	ListCell   *lc;

	appendStringInfoString(buf, "AgentPlan v1\n");
	appendStringInfo(buf, "  Command: %s\n", plan->command_name);

	if (plan->goal != NULL)
		appendStringInfo(buf, "  Goal: %s\n", plan->goal);
	if (plan->identity_name != NULL)
		appendStringInfo(buf, "  Identity: %s\n", plan->identity_name);
	if (plan->namespace_policy_name != NULL)
		appendStringInfo(buf, "  Namespace Policy: %s\n", plan->namespace_policy_name);
	if (plan->task_name != NULL)
		appendStringInfo(buf, "  Task Name: %s\n", plan->task_name);
	if (plan->priority != NULL)
		appendStringInfo(buf, "  Priority: %s\n", plan->priority);
	if (plan->checkpoint_label != NULL)
		appendStringInfo(buf, "  Checkpoint Label: %s\n", plan->checkpoint_label);

	appendStringInfo(buf, "  Input Present: %s\n",
					 plan->input_present ? "true" : "false");
	appendStringInfo(buf, "  Target State: %s\n", plan->target_state);
	appendStringInfo(buf, "  Result Mode: %s\n", plan->result_mode);
	appendStringInfo(buf, "  Checkpointable: %s\n",
					 plan->checkpointable ? "true" : "false");
	appendStringInfo(buf, "  Resumable: %s\n",
					 plan->resumable ? "true" : "false");
	if (plan->budget_tokens > 0)
		appendStringInfo(buf, "  Budget Tokens: %d\n", plan->budget_tokens);
	if (plan->budget_cost > 0)
		appendStringInfo(buf, "  Budget Cost: %d\n", plan->budget_cost);
	appendStringInfo(buf, "  Estimated Tokens: %d\n", plan->estimated_tokens);
	appendStringInfo(buf, "  Estimated Cost: %d\n", plan->estimated_total_cost);
	appendStringInfo(buf, "  Estimated Tool Calls: %d\n",
					 plan->estimated_tool_calls);
	if (plan->authorized_tool_tokens > 0 || plan->authorized_tool_cost > 0)
		appendStringInfo(buf, "  Authorized Tool Budget: tokens=%d cost=%d\n",
						 plan->authorized_tool_tokens,
						 plan->authorized_tool_cost);
	if (plan->authorized_tools != NIL)
	{
		ListCell   *tool_lc;
		bool		first = true;

		appendStringInfoString(buf, "  Authorized Tools: ");
		foreach(tool_lc, plan->authorized_tools)
		{
			char	   *display_contract;

			if (!first)
				appendStringInfoString(buf, ", ");
			display_contract = qx_plan_display_contract(strVal(lfirst(tool_lc)));
			appendStringInfoString(buf, display_contract);
			pfree(display_contract);
			first = false;
		}
		appendStringInfoChar(buf, '\n');
	}
	appendStringInfoString(buf, "  Steps:\n");

	foreach(lc, plan->steps)
	{
		QxAgentPlanStep *step = lfirst(lc);

		appendStringInfo(buf,
						 "    %d. %s [cost=%d, checkpoint=%s, retry=%s]\n",
						 step->seqno,
						 step->name,
						 step->estimated_cost,
						 step->checkpoint_boundary ? "true" : "false",
						 step->retryable ? "true" : "false");
		appendStringInfo(buf, "       %s\n", step->detail);
	}
}

void
QxExplainAgentCommand(ExplainAgentStmt *stmt, DestReceiver *dest)
{
	QxAgentPlan *plan;
	StringInfoData buf;
	TupOutputState *tstate;

	plan = QxBuildAgentPlan(stmt->statement, GetUserId());

	initStringInfo(&buf);
	qx_append_plan_text(&buf, plan);

	tstate = begin_tup_output_tupdesc(dest, QxExplainAgentResultDesc(),
									  &TTSOpsVirtual);
	do_text_output_multiline(tstate, buf.data);
	end_tup_output(tstate);

	pfree(buf.data);
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

static int32
qx_estimate_token_usage(bool input_present, int32 tool_token_cost)
{
	return 32 + (input_present ? 24 : 8) + tool_token_cost;
}
