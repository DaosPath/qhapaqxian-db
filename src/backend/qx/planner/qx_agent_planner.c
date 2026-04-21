/*-------------------------------------------------------------------------
 *
 * qx_agent_planner.c
 *	  Stage 29 capability-aware agent planner for QhapaqXian Engine
 *
 * Copyright (c) 1996-2024, PostgreSQL Global Development Group
 *
 * src/backend/qx/planner/qx_agent_planner.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/tupdesc.h"
#include "catalog/pg_qx_session.h"
#include "catalog/pg_qx_task.h"
#include "commands/defrem.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/value.h"
#include "qx/qx_agent_planner.h"
#include "qx/qx_catalog.h"
#include "qx/qx_security.h"
#include "tcop/tcopprot.h"
#include "utils/acl.h"
#include "utils/builtins.h"

static Oid qx_extract_oid_literal(Node *expr, const char *subject,
								  const char *detail, const char *hint);
static QxAgentPlan *qx_make_agent_plan(QxAgentPlanKind kind, Oid ownerid,
									   const char *command_name);
static void qx_agent_plan_add_step(QxAgentPlan *plan, int16 seqno,
								   const char *name, const char *detail,
								   int32 estimated_cost,
								   bool checkpoint_boundary, bool retryable);
static char *qx_contract_value(const char *contract, const char *key);
static const char *qx_default_runtime_for_provider_kind(const char *provider_kind);
static char *qx_plan_capability_class(const char *provider_kind);
static QxAgentPlanToolDecision *qx_make_tool_decision(const char *contract);
static void qx_plan_add_unique_string(List **list, const char *value);
static void qx_plan_record_tool_decision(QxAgentPlan *plan,
										 QxAgentPlanToolDecision *decision);
static char *qx_plan_runtime_surface_name(const QxAgentPlan *plan);
static void qx_plan_append_string_list(StringInfo buf, const char *label,
									   List *values);
static QxAgentPlan *qx_plan_run_task(RunTaskStmt *stmt, Oid ownerid);
static QxAgentPlan *qx_plan_resume_task(ResumeTaskStmt *stmt, Oid ownerid);
static void qx_append_plan_text(StringInfo buf, const QxAgentPlan *plan);
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
		return pstrdup(start);

	value = palloc(end - start + 1);
	memcpy(value, start, end - start);
	value[end - start] = '\0';
	return value;
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
qx_plan_capability_class(const char *provider_kind)
{
	if (provider_kind == NULL || provider_kind[0] == '\0')
		return pstrdup("unclassified");
	if (strcmp(provider_kind, "loopback") == 0)
		return pstrdup("host-local");
	if (strcmp(provider_kind, "remote") == 0)
		return pstrdup("remote-brokered");
	if (strcmp(provider_kind, "container") == 0)
		return pstrdup("container-brokered");
	if (strcmp(provider_kind, "microvm") == 0)
		return pstrdup("microvm-brokered");
	return psprintf("%s-brokered", provider_kind);
}

static QxAgentPlanToolDecision *
qx_make_tool_decision(const char *contract)
{
	QxAgentPlanToolDecision *decision;
	char	   *provider_kind;
	const char *principal_runtime;
	char	   *provider_attestation;

	decision = palloc0(sizeof(QxAgentPlanToolDecision));
	decision->tool_name = qx_contract_value(contract, "tool");
	decision->handler_name = qx_contract_value(contract, "handler");
	decision->tool_sandbox_name = qx_contract_value(contract, "tool_sandbox");
	decision->principal_name = qx_contract_value(contract, "principal");
	decision->principal_runtime_class = qx_contract_value(contract, "principal_runtime");
	decision->provider_name = qx_contract_value(contract, "provider");
	decision->provider_kind = qx_contract_value(contract, "provider_kind");
	decision->provider_endpoint = qx_contract_value(contract, "provider_endpoint");
	decision->receipt_schema = qx_contract_value(contract, "receipt_schema");
	decision->receipt_alg = qx_contract_value(contract, "receipt_alg");
	decision->attestation_mode = qx_contract_value(contract, "provider_attestation");

	provider_kind = decision->provider_kind != NULL ? decision->provider_kind : "loopback";
	principal_runtime = decision->principal_runtime_class != NULL ?
		decision->principal_runtime_class :
		qx_default_runtime_for_provider_kind(provider_kind);
	provider_attestation = decision->attestation_mode != NULL ?
		decision->attestation_mode : "optional";

	if (decision->principal_runtime_class == NULL)
		decision->principal_runtime_class = pstrdup(principal_runtime);

	decision->capability_class = qx_plan_capability_class(provider_kind);
	decision->require_attestation =
		(provider_attestation != NULL &&
		 strcmp(provider_attestation, "required") == 0);

	return decision;
}

static void
qx_plan_add_unique_string(List **list, const char *value)
{
	ListCell   *lc;

	if (value == NULL || value[0] == '\0')
		return;

	foreach(lc, *list)
	{
		const char *existing = strVal(lfirst(lc));

		if (strcmp(existing, value) == 0)
			return;
	}

	*list = lappend(*list, makeString(pstrdup(value)));
}

static void
qx_plan_record_tool_decision(QxAgentPlan *plan, QxAgentPlanToolDecision *decision)
{
	if (plan == NULL || decision == NULL)
		return;

	plan->tool_decisions = lappend(plan->tool_decisions, decision);
	qx_plan_add_unique_string(&plan->runtime_decision.provider_kinds,
							  decision->provider_kind);
	qx_plan_add_unique_string(&plan->runtime_decision.principal_runtime_classes,
							  decision->principal_runtime_class);
	qx_plan_add_unique_string(&plan->runtime_decision.capability_tags,
							  decision->capability_class);
	if (decision->require_attestation)
		plan->runtime_decision.any_attestation_required = true;
}

static char *
qx_plan_runtime_surface_name(const QxAgentPlan *plan)
{
	bool		multiple_provider_kinds = false;
	bool		multiple_runtime_classes = false;
	char	   *single_provider_kind = NULL;
	char	   *single_runtime_class = NULL;
	ListCell   *lc;

	foreach(lc, plan->runtime_decision.provider_kinds)
	{
		char *provider_kind = strVal(lfirst(lc));

		if (single_provider_kind == NULL)
			single_provider_kind = provider_kind;
		else if (strcmp(single_provider_kind, provider_kind) != 0)
		{
			multiple_provider_kinds = true;
			break;
		}
	}

	foreach(lc, plan->runtime_decision.principal_runtime_classes)
	{
		char *runtime_class = strVal(lfirst(lc));

		if (single_runtime_class == NULL)
			single_runtime_class = runtime_class;
		else if (strcmp(single_runtime_class, runtime_class) != 0)
		{
			multiple_runtime_classes = true;
			break;
		}
	}

	if (plan->tool_decisions == NIL)
		return pstrdup("unclassified");
	if (multiple_provider_kinds || multiple_runtime_classes)
		return pstrdup("mixed-brokered");
	if (single_provider_kind == NULL)
		return pstrdup("unclassified");
	if (strcmp(single_provider_kind, "loopback") == 0)
		return pstrdup("host-local");
	if (strcmp(single_provider_kind, "remote") == 0)
		return pstrdup("remote-brokered");
	if (strcmp(single_provider_kind, "container") == 0)
		return pstrdup("container-brokered");
	if (strcmp(single_provider_kind, "microvm") == 0)
		return pstrdup("microvm-brokered");

	return psprintf("%s-brokered", single_provider_kind);
}

static QxAgentPlan *
qx_plan_run_task(RunTaskStmt *stmt, Oid ownerid)
{
	Oid			sessionoid;
	QxCatalogSessionInfo session;
	QxCatalogAgentInfo agent;
	QxCatalogIdentityInfo identity;
	QxAgentPlan *plan;
	QxBudgetPolicy budget;
	List	   *authorized_tools;
	QxToolAuthorization toolauth;

	MemSet(&session, 0, sizeof(session));
	MemSet(&agent, 0, sizeof(agent));
	MemSet(&identity, 0, sizeof(identity));

	sessionoid = qx_extract_oid_literal(
		stmt->session_id,
		"RUN TASK",
		"Etapa 8 routes RUN TASK through AgentPlan planning, but IN SESSION still does not evaluate arbitrary expressions.",
		"Pass a numeric session OID literal, or resolve the session OID in the client before invoking RUN TASK.");
	if (!QxCatalogLookupSessionByOid(sessionoid, &session))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("session %u does not exist", sessionoid)));

	if (!QxCatalogLookupAgentByOid(session.agentoid, &agent))
	{
		QxCatalogFreeSessionInfo(&session);
		elog(ERROR, "cache lookup failed for QhapaqXian agent %u",
			 session.agentoid);
	}

	if (!QxCatalogLookupIdentityByOid(session.identityoid, &identity))
	{
		QxCatalogFreeAgentInfo(&agent);
		QxCatalogFreeSessionInfo(&session);
		elog(ERROR, "cache lookup failed for QhapaqXian identity %u",
			 session.identityoid);
	}

	if (session.dbid != MyDatabaseId)
	{
		QxCatalogFreeIdentityInfo(&identity);
		QxCatalogFreeAgentInfo(&agent);
		QxCatalogFreeSessionInfo(&session);
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("session %u belongs to a different database", sessionoid)));
	}

	if (session.status != QX_SESSION_STATUS_ACTIVE)
	{
		QxCatalogFreeIdentityInfo(&identity);
		QxCatalogFreeAgentInfo(&agent);
		QxCatalogFreeSessionInfo(&session);
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("session %u is not active", sessionoid),
				 errdetail("RUN TASK in Etapa 8 only accepts sessions in active state.")));
	}

	if (!has_privs_of_role(ownerid, session.ownerid))
	{
		QxCatalogFreeIdentityInfo(&identity);
		QxCatalogFreeAgentInfo(&agent);
		QxCatalogFreeSessionInfo(&session);
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to run a task in session %u",
						sessionoid),
				 errdetail("Only the session owner or a member of that role may enqueue tasks in the session.")));
	}

	QxBudgetPolicyFromSerialized(agent.budget, &budget);
	authorized_tools = QxDeserializeToolList(agent.tools);
	QxValidateToolList(authorized_tools);
	plan = qx_make_agent_plan(QX_AGENT_PLAN_RUN_TASK, ownerid, "RUN TASK");
	plan->sessionoid = sessionoid;
	plan->agentoid = session.agentoid;
	plan->identityoid = session.identityoid;
	plan->namespace_policy_oid = session.namespacepolicyoid;
	plan->identity_name = pstrdup(identity.name);
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
	QxAuthorizeToolsForNamespace(session.namespacepolicyoid,
								 agent.namespaceoid,
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
	{
		ListCell   *tool_lc;

	foreach(tool_lc, plan->authorized_tools)
	{
		QxAgentPlanToolDecision *decision;

		decision = qx_make_tool_decision(strVal(lfirst(tool_lc)));
		qx_plan_record_tool_decision(plan, decision);
	}
	}
	plan->runtime_decision.execution_surface =
		qx_plan_runtime_surface_name(plan);

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
		ereport(ERROR,
				(errcode(ERRCODE_CONFIGURATION_LIMIT_EXCEEDED),
				 errmsg("task plan exceeds the configured cost budget for agent \"%s\"",
						agent.name),
				 errdetail("Estimated cost %d exceeds budget limit %d under identity \"%s\".",
						   plan->estimated_total_cost, budget.cost_limit,
						   identity.name)));
	}

	if (toolauth.enforce_budgets &&
		budget.has_token_limit &&
		plan->estimated_tokens > budget.token_limit)
	{
		ereport(ERROR,
				(errcode(ERRCODE_CONFIGURATION_LIMIT_EXCEEDED),
				 errmsg("task plan exceeds the configured token budget for agent \"%s\"",
						agent.name),
				 errdetail("Estimated token usage %d exceeds budget limit %d under identity \"%s\".",
						   plan->estimated_tokens, budget.token_limit,
						   identity.name)));
	}

	QxCatalogFreeIdentityInfo(&identity);
	QxCatalogFreeAgentInfo(&agent);
	QxCatalogFreeSessionInfo(&session);

	return plan;
}

static QxAgentPlan *
qx_plan_resume_task(ResumeTaskStmt *stmt, Oid ownerid)
{
	Oid			taskoid;
	QxCatalogTaskInfo task;
	QxCatalogCheckpointInfo checkpoint;
	const char *stored_label;
	QxAgentPlan *plan;

	MemSet(&task, 0, sizeof(task));
	MemSet(&checkpoint, 0, sizeof(checkpoint));

	taskoid = qx_extract_oid_literal(
		stmt->task_id,
		"RESUME TASK",
		"Etapa 8 keeps RESUME TASK on the utility/planner path and does not yet evaluate arbitrary expressions in TASK identifiers.",
		"Pass a numeric task OID literal, or resolve the task OID in the client before invoking RESUME TASK.");

	if (!QxCatalogLookupTaskByOid(taskoid, &task))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("task %u does not exist", taskoid)));

	if (!has_privs_of_role(ownerid, task.ownerid))
	{
		QxCatalogFreeTaskInfo(&task);
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied to resume task %u", taskoid),
				 errdetail("Only the task owner or a member of that role may resume the task.")));
	}

	if (task.state != QX_TASK_STATE_CHECKPOINTED)
	{
		QxCatalogFreeTaskInfo(&task);
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("task %u is not resumable", taskoid),
				 errdetail("RESUME TASK only accepts tasks in checkpointed state.")));
	}

	if (!OidIsValid(task.lastcheckpointid))
	{
		QxCatalogFreeTaskInfo(&task);
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("task %u has no checkpoint to resume from", taskoid)));
	}

	if (!QxCatalogLookupCheckpointByOid(task.lastcheckpointid, &checkpoint))
	{
		QxCatalogFreeTaskInfo(&task);
		elog(ERROR, "cache lookup failed for QhapaqXian checkpoint %u",
			 task.lastcheckpointid);
	}

	stored_label = checkpoint.label;

	if (stmt->checkpoint_label != NULL &&
		(stored_label == NULL || strcmp(stmt->checkpoint_label, stored_label) != 0))
	{
		QxCatalogFreeCheckpointInfo(&checkpoint);
		QxCatalogFreeTaskInfo(&task);
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("checkpoint label \"%s\" does not match the current resumable checkpoint for task %u",
						stmt->checkpoint_label, taskoid)));
	}

	plan = qx_make_agent_plan(QX_AGENT_PLAN_RESUME_TASK, ownerid, "RESUME TASK");
	plan->taskoid = taskoid;
	plan->sessionoid = task.sessionoid;
	plan->agentoid = task.agentoid;
	plan->namespace_policy_oid = task.namespacepolicyoid;
	plan->identityoid = task.identityoid;
	plan->checkpointoid = task.lastcheckpointid;
	plan->identity_name = task.identity_name != NULL ?
		pstrdup(task.identity_name) :
		QxIdentityNameById(task.identityoid);
	plan->namespace_policy_name = task.policy_name != NULL ?
		pstrdup(task.policy_name) :
		QxNamespacePolicyNameById(task.namespacepolicyoid);
	plan->task_name = NULL;
	plan->goal = NULL;
	plan->priority = NULL;
	plan->checkpoint_label = pstrdup(stored_label != NULL ? stored_label : "<unnamed>");
	plan->target_state = pstrdup("completed");
	plan->result_mode = pstrdup("state transition via pg_qx_task");
	plan->input_present = false;
	plan->checkpointable = true;
	plan->resumable = true;
	plan->budget_tokens = task.budget_tokens;
	plan->budget_cost = task.budget_cost;
	plan->estimated_tokens = task.estimated_tokens;
	plan->estimated_total_cost = task.estimated_cost;
	plan->authorized_tools = QxDeserializeToolList(task.authorized_tools);
	plan->estimated_tool_calls = list_length(plan->authorized_tools);
	plan->authorized_tool_tokens = task.authorized_tool_tokens;
	plan->authorized_tool_cost = task.authorized_tool_cost;
	{
		ListCell   *tool_lc;

	foreach(tool_lc, plan->authorized_tools)
	{
		QxAgentPlanToolDecision *decision;

		decision = qx_make_tool_decision(strVal(lfirst(tool_lc)));
		qx_plan_record_tool_decision(plan, decision);
	}
	}
	plan->runtime_decision.execution_surface =
		qx_plan_runtime_surface_name(plan);

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

	QxCatalogFreeCheckpointInfo(&checkpoint);
	QxCatalogFreeTaskInfo(&task);

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
	if (plan->runtime_decision.execution_surface != NULL)
	{
		appendStringInfo(buf, "  Execution Surface: %s\n",
						 plan->runtime_decision.execution_surface);
	}
	if (plan->runtime_decision.provider_kinds != NIL)
		qx_plan_append_string_list(buf, "  Provider Kinds: ",
								   plan->runtime_decision.provider_kinds);
	if (plan->runtime_decision.principal_runtime_classes != NIL)
		qx_plan_append_string_list(buf, "  Principal Runtime Classes: ",
								   plan->runtime_decision.principal_runtime_classes);
	if (plan->runtime_decision.capability_tags != NIL)
		qx_plan_append_string_list(buf, "  Capability Tags: ",
								   plan->runtime_decision.capability_tags);
	appendStringInfo(buf, "  Any Attestation Required: %s\n",
					 plan->runtime_decision.any_attestation_required ? "true" : "false");
	if (plan->tool_decisions != NIL)
	{
		ListCell   *decision_lc;

		appendStringInfoString(buf, "  Tool Decisions:\n");
		foreach(decision_lc, plan->tool_decisions)
		{
			QxAgentPlanToolDecision *decision = lfirst(decision_lc);

			appendStringInfo(buf,
							 "    - tool=%s;handler=%s;principal=%s;principal_runtime=%s;provider=%s;provider_kind=%s;provider_endpoint=%s;sandbox=%s;receipt_schema=%s;receipt_alg=%s;attestation=%s;capability=%s\n",
							 decision->tool_name != NULL ? decision->tool_name : "<unknown>",
							 decision->handler_name != NULL ? decision->handler_name : "<unknown>",
							 decision->principal_name != NULL ? decision->principal_name : "<unknown>",
							 decision->principal_runtime_class != NULL ? decision->principal_runtime_class : "<unknown>",
							 decision->provider_name != NULL ? decision->provider_name : "<unknown>",
							 decision->provider_kind != NULL ? decision->provider_kind : "<unknown>",
							 decision->provider_endpoint != NULL ? decision->provider_endpoint : "<unknown>",
							 decision->tool_sandbox_name != NULL ? decision->tool_sandbox_name : "<unknown>",
							 decision->receipt_schema != NULL ? decision->receipt_schema : "<unknown>",
							 decision->receipt_alg != NULL ? decision->receipt_alg : "<unknown>",
							 decision->attestation_mode != NULL ? decision->attestation_mode : "optional",
							 decision->capability_class != NULL ? decision->capability_class : "unclassified");
		}
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

static int32
qx_estimate_token_usage(bool input_present, int32 tool_token_cost)
{
	return 32 + (input_present ? 24 : 8) + tool_token_cost;
}

static void
qx_plan_append_string_list(StringInfo buf, const char *label, List *values)
{
	ListCell   *lc;
	bool		first = true;

	appendStringInfoString(buf, label);
	foreach(lc, values)
	{
		if (!first)
			appendStringInfoString(buf, ", ");
		appendStringInfoString(buf, strVal(lfirst(lc)));
		first = false;
	}
	appendStringInfoChar(buf, '\n');
}
