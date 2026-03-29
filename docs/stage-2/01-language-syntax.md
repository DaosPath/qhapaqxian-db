# Etapa 2: Sintaxis del Lenguaje Agentic v0

## Alcance

Esta especificacion define la superficie sintactica inicial del lenguaje nativo de QhapaqXian DB para manejar agentes como entidades de primer nivel del motor.

Lo que cubre:
- definicion de agentes
- inicio de sesiones
- lanzamiento de tareas
- memoria persistente basica
- checkpoint y reanudacion
- inspeccion de trazas
- explicacion de planes agentic

Lo que no cubre aun:
- parser y analisis en `gram.y`
- catlogos fisicos
- runtime interno real
- scheduler
- planner agent-aware
- WAL y replicacion semantica

## Filosofia

El lenguaje debe ser:
- operativo, no narrativo
- explicito en identidad, sesion y tarea
- compatible con SQL donde sea util
- determinista en estructura, aunque no en el contenido semantico interno de la razon
- apto para ser implementado como primitivas de motor, no como funciones sueltas

Principios de diseno:
- `CREATE AGENT` define una entidad duradera.
- `START SESSION` abre contexto operativo.
- `RUN TASK` crea una ejecucion con ciclo de vida propio.
- `REMEMBER` y `FETCH MEMORY` operan sobre memoria del agente, no sobre tablas genericas.
- `CHECKPOINT TASK` y `RESUME TASK` expresan persistencia de progreso.
- `SHOW TRACE` y `EXPLAIN AGENT` son introspeccion nativa.

## Comandos Iniciales

### `CREATE AGENT`

Declara un agente con identidad, modelo, memoria, herramientas, politica y presupuesto.

```sql
CREATE AGENT analyst
  IDENTITY svc_analyst
  MODEL 'builtin://reasoner/default'
  MEMORY PROFILE default_mem
  TOOLS (sql_readonly, doc_fetch)
  POLICY strict_default
  BUDGET TOKENS 200000 COST_LIMIT 25.00 TIME_LIMIT '30 min';
```

### `ALTER AGENT`

Modifica atributos controlados de un agente existente.

```sql
ALTER AGENT analyst
  SET MODEL 'builtin://reasoner/v2'
  ADD TOOL summary_tool
  DROP TOOL sql_readonly;
```

### `START SESSION`

Abre una sesion operativa para un agente.

```sql
START SESSION FOR AGENT analyst
  WITH CONTEXT '{"ticket":"INC-42","tenant":"acme"}'
  RETURNING SESSION;
```

### `RUN TASK`

Lanza una tarea dentro de una sesion activa.

```sql
RUN TASK diagnose_incident
  IN SESSION 1001
  GOAL 'diagnosticar la causa raiz y proponer recuperacion'
  INPUT '{"incident_id":"INC-42"}'
  PRIORITY HIGH
  RETURNING TASK;
```

### `REMEMBER`

Guarda memoria persistente del agente o de la sesion.

```sql
REMEMBER
  IN SESSION 1001
  SCOPE EPISODIC
  KEY 'incident.root_cause'
  VALUE '{"cause":"partition_missing"}'
  TAGS ('incident','etl');
```

### `FETCH MEMORY`

Consulta memoria por alcance y similitud o filtros semanticos.

```sql
FETCH MEMORY
  FOR AGENT analyst
  SCOPE EPISODIC, SEMANTIC
  MATCH 'partition missing in etl'
  LIMIT 10;
```

### `CHECKPOINT TASK`

Materializa un punto recuperable de la tarea.

```sql
CHECKPOINT TASK 7001
  LABEL 'before_retry';
```

### `RESUME TASK`

Reanuda una tarea desde un checkpoint previo.

```sql
RESUME TASK 7001
  FROM CHECKPOINT 'before_retry';
```

### `SHOW TRACE`

Inspecciona trazas de una tarea.

```sql
SHOW TRACE FOR TASK 7001
  LIMIT 100;
```

### `EXPLAIN AGENT`

Explica el plan agentic propuesto para una tarea.

```sql
EXPLAIN AGENT
RUN TASK diagnose_incident
  IN SESSION 1001
  GOAL 'diagnosticar la causa raiz y proponer recuperacion'
  INPUT '{"incident_id":"INC-42"}';
```

## EBNF Util

```ebnf
AgentStmt           ::= CreateAgentStmt
                      | AlterAgentStmt
                      | StartSessionStmt
                      | RunTaskStmt
                      | RememberStmt
                      | FetchMemoryStmt
                      | CheckpointTaskStmt
                      | ResumeTaskStmt
                      | ShowTraceStmt
                      | ExplainAgentStmt ;

CreateAgentStmt     ::= "CREATE" "AGENT" ident
                        OptAgentIdentity
                        OptAgentModel
                        OptAgentMemoryProfile
                        OptAgentTools
                        OptAgentPolicy
                        OptAgentBudget
                        ";" ;

AlterAgentStmt      ::= "ALTER" "AGENT" ident AlterAgentCmdList ";" ;

StartSessionStmt    ::= "START" "SESSION" "FOR" "AGENT" qual_name
                        OptSessionContext
                        OptReturningClause
                        ";" ;

RunTaskStmt         ::= "RUN" "TASK" opt_ident
                        "IN" "SESSION" a_expr
                        "GOAL" string_literal
                        OptTaskInput
                        OptTaskPriority
                        OptReturningClause
                        ";" ;

RememberStmt        ::= "REMEMBER" "IN" "SESSION" a_expr
                        "SCOPE" MemoryScope
                        "KEY" string_literal
                        "VALUE" a_expr
                        OptTagsClause
                        ";" ;

FetchMemoryStmt     ::= "FETCH" "MEMORY" "FOR" "AGENT" qual_name
                        "SCOPE" MemoryScopeList
                        "MATCH" a_expr
                        OptLimitClause
                        ";" ;

CheckpointTaskStmt  ::= "CHECKPOINT" "TASK" a_expr
                        OptCheckpointLabel
                        ";" ;

ResumeTaskStmt      ::= "RESUME" "TASK" a_expr
                        "FROM" "CHECKPOINT" string_literal
                        ";" ;

ShowTraceStmt       ::= "SHOW" "TRACE" "FOR" "TASK" a_expr
                        OptLimitClause
                        ";" ;

ExplainAgentStmt    ::= "EXPLAIN" "AGENT" AgentStmt ;

OptAgentIdentity    ::= [ "IDENTITY" ident ] ;
OptAgentModel       ::= [ "MODEL" string_literal ] ;
OptAgentMemoryProfile ::= [ "MEMORY" "PROFILE" ident ] ;
OptAgentTools       ::= [ "TOOLS" "(" ident { "," ident } ")" ] ;
OptAgentPolicy      ::= [ "POLICY" ident ] ;
OptAgentBudget      ::= [ "BUDGET" "TOKENS" integer
                          [ "COST_LIMIT" numeric ]
                          [ "TIME_LIMIT" interval_literal ] ] ;
OptSessionContext   ::= [ "WITH" "CONTEXT" json_literal ] ;
OptReturningClause  ::= [ "RETURNING" ( "SESSION" | "TASK" ) ] ;
OptTaskInput        ::= [ "INPUT" json_literal ] ;
OptTaskPriority     ::= [ "PRIORITY" ( "LOW" | "NORMAL" | "HIGH" | "CRITICAL" ) ] ;
OptTagsClause       ::= [ "TAGS" "(" string_literal { "," string_literal } ")" ] ;
OptLimitClause      ::= [ "LIMIT" integer ] ;
OptCheckpointLabel  ::= [ "LABEL" string_literal ] ;
MemoryScope         ::= "WORKING" | "EPISODIC" | "SEMANTIC" ;
MemoryScopeList     ::= MemoryScope { "," MemoryScope } ;
AlterAgentCmdList   ::= AlterAgentCmd { AlterAgentCmd } ;
AlterAgentCmd       ::= "SET" "MODEL" string_literal
                      | "SET" "POLICY" ident
                      | "ADD" "TOOL" ident
                      | "DROP" "TOOL" ident ;
```

## Semantica Basica

### `CREATE AGENT`
- crea una definicion duradera de agente
- reserva identidad logica
- fija politica, herramientas y presupuesto inicial

### `ALTER AGENT`
- cambia solo atributos permitidos
- no altera tareas en curso salvo que la politica futura lo permita

### `START SESSION`
- crea una sesion ligada a un agente
- hereda politica y limites por defecto
- genera contexto operativo inicial

### `RUN TASK`
- crea una tarea con objetivo, entrada y prioridad
- la sesion debe existir y estar activa
- la tarea debe quedar asociada a un agente visible y autorizado

### `REMEMBER`
- persiste memoria con alcance explicito
- `WORKING` es temporal y de alta mutacion
- `EPISODIC` guarda eventos o hechos de ejecucion
- `SEMANTIC` se reserva para recuperacion conceptual

### `FETCH MEMORY`
- lee memoria segun alcance, filtros y limite
- no modifica estado

### `CHECKPOINT TASK`
- materializa progreso recuperable
- no implica commit global del flujo de trabajo

### `RESUME TASK`
- reabre una tarea desde un checkpoint valido
- requiere un punto de reanudacion consistente

### `SHOW TRACE`
- consulta trazas de ejecucion y observabilidad

### `EXPLAIN AGENT`
- describe el plan agentic propuesto
- no ejecuta la tarea

## Validaciones Semanticas Minimas

- `CREATE AGENT` requiere identidad unica y politica valida.
- `START SESSION` requiere agente existente y autorizacion.
- `RUN TASK` requiere sesion activa.
- `REMEMBER` requiere `SCOPE` valido y valor serializable.
- `FETCH MEMORY` requiere scope visible para el agente.
- `CHECKPOINT TASK` requiere tarea en estado checkpointable.
- `RESUME TASK` requiere checkpoint existente y tarea recuperable.
- `SHOW TRACE` requiere tarea existente o visible.
- `EXPLAIN AGENT` solo acepta sentencias agentic validas.

## Errores Principales

La Etapa 2 define la superficie de errores, aunque los `SQLSTATE` finales se fijan en Etapa 3.

Propuesta inicial:
- `ERRCODE_UNDEFINED_AGENT`
- `ERRCODE_UNDEFINED_AGENT_IDENTITY`
- `ERRCODE_AGENT_POLICY_VIOLATION`
- `ERRCODE_AGENT_BUDGET_EXCEEDED`
- `ERRCODE_INVALID_AGENT_STATE`
- `ERRCODE_INVALID_SESSION_STATE`
- `ERRCODE_INVALID_TASK_STATE`
- `ERRCODE_INVALID_CHECKPOINT`
- `ERRCODE_AGENT_TOOL_NOT_ALLOWED`
- `ERRCODE_AGENT_NAMESPACE_VIOLATION`
- `ERRCODE_AGENT_RESUME_CONFLICT`

## MVP vs Diferido

### MVP de Etapa 2
- sintaxis estable de los 10 comandos base
- EBNF util para parser y analisis posterior
- ejemplos SQL canonicos
- semantica publica minima por comando
- lista inicial de errores

### Diferido a Etapa 3
- parser real en `gram.y`
- scanner y keywords en `scan.l`
- AST concreto en `agentnodes.h`
- `ProcessUtility` y `cmdtaglist.h`
- validaciones codificadas en backend

### Diferido a Etapa 4+
- catalogos persistentes
- runtime interno
- scheduler
- planner/executor agent-aware
- WAL y replicacion semantica

## Criterio de Salida Hacia Etapa 3

Esta etapa esta completa cuando existan:
- sintaxis congelada para los comandos iniciales
- semantica de alto nivel aprobada
- errores y estados principales definidos
- decision explicita sobre keywords y conflicto SQL
- handoff claro para `gram.y`, `scan.l`, AST y `cmdtaglist.h`

Si cambia la sintaxis despues de este punto, el costo de rebase a parser y tests aumenta de forma material y debe tratarse como cambio de arquitectura, no como ajuste menor.
