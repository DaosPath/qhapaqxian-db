/*-------------------------------------------------------------------------
 *
 * qx_agent_executor.c
 *	  Stage 29 capability-aware agent executor boundary for QhapaqXian Engine
 *
 * Copyright (c) 1996-2024, PostgreSQL Global Development Group
 *
 * src/backend/qx/executor/qx_agent_executor.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "nodes/value.h"
#include "qx/qx_agent_executor.h"
#include "qx/qx_runtime.h"

static bool qx_plan_runtime_binding_is_valid(const char *provider_kind,
											 const char *principal_runtime_class);
static int	qx_plan_route_rank(const QxAgentPlanToolDecision *decision);
static const char *qx_resolve_plan_phase_contract(const QxAgentPlan *plan,
												 bool prefer_resume);
static void qx_validate_agent_plan_capabilities(const QxAgentPlan *plan);

static bool
qx_plan_runtime_binding_is_valid(const char *provider_kind,
								 const char *principal_runtime_class)
{
	if (provider_kind == NULL || principal_runtime_class == NULL)
		return false;

	if ((strcmp(provider_kind, "loopback") == 0 ||
		 strcmp(provider_kind, "remote") == 0) &&
		strcmp(principal_runtime_class, "host") == 0)
		return true;
	if (strcmp(provider_kind, "container") == 0 &&
		strcmp(principal_runtime_class, "container") == 0)
		return true;
	if (strcmp(provider_kind, "microvm") == 0 &&
		strcmp(principal_runtime_class, "microvm") == 0)
		return true;

	return false;
}

static void
qx_validate_agent_plan_contract_alignment(const QxAgentPlan *plan)
{
	if (list_length(plan->tool_decisions) != list_length(plan->authorized_tools))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("agent plan capability decisions do not align with authorized tools"),
				 errdetail("Stage 29 phase routing requires one structured tool decision per authorized contract.")));
}

static int
qx_plan_route_rank(const QxAgentPlanToolDecision *decision)
{
	const char *capability_class;
	const char *provider_kind;
	const char *runtime_class;

	if (decision == NULL)
		return 1;

	capability_class = decision->capability_class;
	provider_kind = decision->provider_kind;
	runtime_class = decision->principal_runtime_class;

	if (capability_class != NULL)
	{
		if (strcmp(capability_class, "host-local") == 0)
			return 0;
		if (strcmp(capability_class, "remote-brokered") == 0)
			return 1;
		if (strcmp(capability_class, "container-brokered") == 0)
			return 2;
		if (strcmp(capability_class, "microvm-brokered") == 0)
			return 3;
	}

	if (runtime_class != NULL)
	{
		if (strcmp(runtime_class, "host") == 0)
			return 0;
		if (strcmp(runtime_class, "container") == 0)
			return 2;
		if (strcmp(runtime_class, "microvm") == 0)
			return 3;
	}

	if (provider_kind != NULL)
	{
		if (strcmp(provider_kind, "loopback") == 0)
			return 0;
		if (strcmp(provider_kind, "remote") == 0)
			return 1;
		if (strcmp(provider_kind, "container") == 0)
			return 2;
		if (strcmp(provider_kind, "microvm") == 0)
			return 3;
	}

	return 1;
}

static const char *
qx_resolve_plan_phase_contract(const QxAgentPlan *plan, bool prefer_resume)
{
	ListCell   *decision_lc;
	ListCell   *contract_lc;
	const char *selected_contract = NULL;
	int			selected_rank = prefer_resume ? PG_INT32_MIN : PG_INT32_MAX;
	int			selected_index = prefer_resume ? PG_INT32_MIN : PG_INT32_MAX;
	int			index = 0;

	if (plan == NULL || plan->tool_decisions == NIL || plan->authorized_tools == NIL)
		return NULL;

	qx_validate_agent_plan_contract_alignment(plan);

	forboth(decision_lc, plan->tool_decisions, contract_lc, plan->authorized_tools)
	{
		QxAgentPlanToolDecision *decision = lfirst(decision_lc);
		const char *contract = strVal(lfirst(contract_lc));
		int			rank = qx_plan_route_rank(decision);
		bool		better;

		if (selected_contract == NULL)
			better = true;
		else if (prefer_resume)
			better = (rank > selected_rank ||
					  (rank == selected_rank && index > selected_index));
		else
			better = (rank < selected_rank ||
					  (rank == selected_rank && index < selected_index));

		if (better)
		{
			selected_contract = contract;
			selected_rank = rank;
			selected_index = index;
		}

		index++;
	}

	return selected_contract;
}

static void
qx_validate_agent_plan_capabilities(const QxAgentPlan *plan)
{
	ListCell   *lc;

	if (plan == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("agent plan must not be null")));

	if (plan->tool_decisions == NIL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("agent plan does not contain capability decisions"),
				 errdetail("Stage 29 requires the planner to populate structured tool decisions before execution.")));

	if (plan->runtime_decision.execution_surface == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("agent plan does not contain a runtime execution surface"),
				 errdetail("Stage 29 requires a planner-populated execution surface summary before execution.")));

	qx_validate_agent_plan_contract_alignment(plan);

	foreach(lc, plan->tool_decisions)
	{
		QxAgentPlanToolDecision *decision = lfirst(lc);

		if (decision == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("agent plan contains a null tool decision")));

		if (!qx_plan_runtime_binding_is_valid(decision->provider_kind,
											  decision->principal_runtime_class))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("tool \"%s\" has an invalid provider/runtime-class binding",
							decision->tool_name != NULL ? decision->tool_name : "<unknown>"),
					 errdetail("Provider kind \"%s\" is not compatible with principal runtime class \"%s\".",
							   decision->provider_kind != NULL ? decision->provider_kind : "<null>",
							   decision->principal_runtime_class != NULL ? decision->principal_runtime_class : "<null>")));

		if (decision->tool_name == NULL || decision->handler_name == NULL ||
			decision->principal_name == NULL || decision->provider_name == NULL ||
			decision->provider_endpoint == NULL || decision->capability_class == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("tool decision for \"%s\" is incomplete",
							decision->tool_name != NULL ? decision->tool_name : "<unknown>"),
					 errdetail("The planner must record handler, principal, provider, provider endpoint, and capability class for Stage 29 execution.")));

		if (decision->require_attestation &&
			(decision->receipt_schema == NULL || decision->receipt_schema[0] == '\0' ||
			 decision->receipt_alg == NULL || decision->receipt_alg[0] == '\0'))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("tool \"%s\" requires receipt attestation metadata",
							decision->tool_name != NULL ? decision->tool_name : "<unknown>"),
					 errdetail("Structured capability planning must preserve both receipt schema and algorithm for attested tools.")));
	}
}

Oid
QxExecuteAgentPlan(const QxAgentPlan *plan)
{
	qx_validate_agent_plan_capabilities(plan);

	switch (plan->kind)
	{
		case QX_AGENT_PLAN_RUN_TASK:
			{
				QxRuntimeTaskRequest request;

				memset(&request, 0, sizeof(request));
				request.sessionoid = plan->sessionoid;
				request.agentoid = plan->agentoid;
				request.identityoid = plan->identityoid;
				request.namespace_policy_oid = plan->namespace_policy_oid;
				request.ownerid = plan->ownerid;
				request.task_name = plan->task_name;
				request.identity_name = plan->identity_name;
				request.namespace_policy_name = plan->namespace_policy_name;
				request.goal = plan->goal;
				request.input = plan->input;
				request.priority = plan->priority;
				request.authorized_tools = plan->authorized_tools;
				request.authorized_tool_oids = plan->authorized_tool_oids;
				request.submit_contract =
					qx_resolve_plan_phase_contract(plan, false);
				request.resume_contract =
					qx_resolve_plan_phase_contract(plan, true);
				request.budget_tokens = plan->budget_tokens;
				request.budget_cost = plan->budget_cost;
				request.estimated_tokens = plan->estimated_tokens;
				request.estimated_cost = plan->estimated_total_cost;
				request.authorized_tool_tokens = plan->authorized_tool_tokens;
				request.authorized_tool_cost = plan->authorized_tool_cost;

				return QxRuntimeSubmitTask(&request);
			}

		case QX_AGENT_PLAN_RESUME_TASK:
			return QxRuntimeResumeTask(plan->taskoid,
									   plan->checkpoint_label,
									   plan->ownerid);
	}

	elog(ERROR, "unrecognized QhapaqXian plan kind: %d", (int) plan->kind);
	return InvalidOid;
}
