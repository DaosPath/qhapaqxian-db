# Etapa 2 a Etapa 3 Handoff

Este documento es la guia ejecutable para pasar de la especificacion del lenguaje agentic v0 a la implementacion real del parser, el analisis y el despacho de sentencias en Etapa 3.

Alcance:
- Etapa 2 entrega la definicion formal del lenguaje.
- Etapa 3 convierte esa definicion en sintaxis, AST, validacion semantica y command dispatch dentro del fork.
- No se toca storage profundo, runtime completo ni planner agent-aware en esta fase.

## Entregables de Etapa 2

1. Especificacion de sintaxis del lenguaje agentic v0.
2. Contrato semantico por comando.
3. Politica inicial de errores y SQLSTATEs.
4. Estrategia de keywords y preparacion del parser.
5. Matriz de compatibilidad por comando.
6. Criterios de salida para iniciar Etapa 3 sin ambiguedad.

## Criterios de salida de Etapa 2

Etapa 2 solo se considera cerrada si existen estos puntos:
- los comandos objetivos estan congelados como superficie publica v0;
- cada comando tiene precondiciones, efectos y errores esperados;
- existe una lista de keywords candidatas y su clasificacion inicial;
- existe una decision explicita de que queda stub y que no;
- existe un inventario de archivos a tocar en Etapa 3;
- el equipo puede empezar `gram.y`, `scan.l` y `agentnodes.h` sin redisenar la semantica.

## Archivos a tocar en Etapa 3

Prioridad alta:
- `src/backend/parser/scan.l`
- `src/backend/parser/gram.y`
- `src/include/parser/kwlist.h`
- `src/include/nodes/agentnodes.h`
- `src/backend/nodes/copyfuncs.c`
- `src/backend/nodes/equalfuncs.c`
- `src/backend/nodes/outfuncs.c`
- `src/backend/nodes/readfuncs.c`
- `src/backend/tcop/utility.c`
- `src/include/tcop/cmdtaglist.h`

Prioridad media:
- `src/backend/parser/analyze.c`
- `src/backend/commands/agentcmds.c`
- `src/backend/commands/sessioncmds.c`
- `src/backend/commands/taskcmds.c`
- `src/include/commands/agentcmds.h`
- `src/include/commands/sessioncmds.h`
- `src/include/commands/taskcmds.h`
- `src/backend/catalog/system_views.sql`

Prioridad baja:
- `src/backend/utils/cache/syscache.c`
- `src/include/utils/syscache.h`
- `src/include/catalog/pg_agent.h`
- `src/include/catalog/pg_agent_identity.h`
- `src/include/catalog/pg_agent_namespace.h`
- `src/include/catalog/pg_agent_policy.h`
- `src/include/catalog/pg_agent_tool.h`

## Orden de implementacion recomendado

1. Keywords y scanner.
2. AST dedicado.
3. Grammar de sentencias.
4. Command tags y utility dispatch.
5. Validacion semantica minima.
6. Handlers stub con efectos acotados.
7. Pruebas de parser y comandos.
8. Vistas administrativas minimas.

## Comandos y prioridades

### 1. `CREATE AGENT`

Prioridad: alta.

Motivo:
- define la entidad base del fork;
- obliga a fijar identidad, policy, tools y budget desde el inicio;
- justifica la necesidad de catalogos propios.

Etapa 3 debe implementar:
- parseo;
- AST;
- validacion basica de campos requeridos;
- despacho a handler;
- completion tag o resultado basico.

Puede quedar stub:
- persistencia final de todos los subcomponentes del agente;
- integracion con runtime real;
- resolucion avanzada de tools o policies.

### 2. `START SESSION`

Prioridad: alta.

Motivo:
- introduce la unidad operativa de ejecucion;
- permite preparar el estado durable de una tarea agentic;
- da una primera forma de identidad operativa.

Etapa 3 debe implementar:
- parseo;
- AST;
- vinculacion a agente existente o valido;
- retorno de `session_id` o equivalente;
- validacion de namespace o ownership basico.

Puede quedar stub:
- scheduler real;
- heartbeats;
- lease semantics;
- reanudacion completa.

### 3. `RUN TASK`

Prioridad: alta.

Motivo:
- es la primera operacion que valida la direccion del fork hacia tareas multi-etapa;
- obliga a separar el lenguaje operativo del SQL relacional clasico;
- prepara la futura maquina de estados.

Etapa 3 debe implementar:
- parseo;
- AST;
- validacion de `session`;
- validacion de `goal` e `input`;
- creacion de registro de task y attempt inicial;
- retorno de `task_id` o equivalente.

Puede quedar stub:
- ejecucion real de pasos;
- branching;
- retries;
- checkpoints durables;
- runtime distribuido.

### 4. `REMEMBER`

Prioridad: media.

Motivo:
- valida el contrato de memoria persistente;
- prepara el espacio para working memory y episodic memory;
- fija la semantica de escritura agentic.

Etapa 3 puede dejarlo como stub parcial si hace falta:
- aceptando parseo y validacion;
- persistiendo una fila simple de memoria;
- sin busqueda semantica avanzada.

### 5. `FETCH MEMORY`

Prioridad: media.

Motivo:
- valida lectura semantica minima;
- prepara el contrato de retrieval;
- no exige planner avanzado aun.

Etapa 3 puede implementar:
- filtrado basico por agent/session/scope;
- busqueda textual o metadatos.

Puede quedar stub:
- embeddings;
- ANN;
- ranking semantico avanzado.

### 6. `CHECKPOINT TASK`

Prioridad: media.

Motivo:
- fuerza la definicion de progreso durable;
- prepara la semantica de resume y recovery;
- separa progreso de commit SQL.

Etapa 3 puede dejarlo como stub funcional:
- registrar checkpoint minimo;
- asociar checkpoint a task/attempt;
- sin recovery real completo aun.

### 7. `RESUME TASK`

Prioridad: media.

Motivo:
- valida la frontera entre control de flujo y recovery;
- fuerza un estado recuperable;
- define el contrato para Etapa 4/6.

Etapa 3 puede dejarlo parcial:
- aceptar sintaxis y validacion;
- verificar existencia del checkpoint;
- no reanudar ejecucion completa todavia.

### 8. `SHOW TRACE`

Prioridad: baja-media.

Motivo:
- valida observabilidad minima;
- permite inspeccion manual del flujo agentic;
- ayuda a depurar Etapa 3.

Etapa 3 puede implementar:
- lectura de una tabla o vista interna;
- limite basico;
- formato simple de salida.

### 9. `EXPLAIN AGENT`

Prioridad: media.

Motivo:
- fija la semantica de introspeccion del plan agentic;
- evita que la futura Etapa 8 parezca un invento posterior;
- permite ver metadata de la ejecucion, aunque al principio sea simple.

Etapa 3 puede dejarlo como stub informativo:
- mostrar estructura basica del comando;
- listar campos y decisiones iniciales;
- sin costeo real aun.

## Tests a crear

### Parser
- casos positivos por comando.
- casos negativos por tokens y clausulas faltantes.
- conflictos con SQL normal.
- roundtrip basico de AST si aplica.

### Comandos
- `CREATE AGENT` con campos requeridos.
- `START SESSION` con agente valido.
- `RUN TASK` con sesion valida.
- `REMEMBER` y `FETCH MEMORY` con scopes validos.
- `CHECKPOINT TASK` y `RESUME TASK` con estados coherentes.

### Compatibilidad
- SQL normal sin cambios de comportamiento.
- keywords nuevas no deben romper sintaxis existente sin justificacion.
- `ProcessUtility` debe seguir enroutando comandos no agentic sin regresiones.

### Recovery y runtime
- no aplican como cobertura completa en Etapa 3, pero si deben existir pruebas de stubs y de invariantes de estado.

## Lo que puede quedar stub

Puede quedar stub en Etapa 3:
- ejecucion real de tools;
- scheduler completo;
- background workers especializados;
- storage semantico profundo;
- planner agent-aware;
- replicacion semantica;
- recovery completo de tasks largas.

No puede quedar stub:
- grammar;
- AST;
- keywords;
- command tags;
- validacion minima;
- dispatch basico;
- errores principales;
- contrato semantico publico.

## Lo que no puede quedar stub

Estas piezas deben existir y ser coherentes antes de cerrar Etapa 3:
- `gram.y`
- `scan.l`
- `kwlist.h`
- `agentnodes.h`
- `cmdtaglist.h`
- `utility.c`
- `analyze.c`
- handlers iniciales de `CREATE AGENT`, `START SESSION` y `RUN TASK`

## Criterio de salida hacia Etapa 3

Etapa 3 puede empezar cuando:
- la sintaxis v0 esta congelada;
- el contrato semantico por comando esta escrito;
- los SQLSTATEs principales estan listados;
- la estrategia de keywords esta decidida;
- el equipo sabe que sera stub y que sera real;
- existe este handoff como referencia de implementacion.

## Criterio de salida de Etapa 3

Etapa 3 solo se considera cerrada cuando:
- los tres comandos base parsean y llegan al dispatcher;
- `CREATE AGENT`, `START SESSION` y `RUN TASK` tienen handlers funcionales mínimos;
- los tests de parser y comandos pasan;
- no se rompen consultas SQL normales;
- la semantica publica coincide con la documentacion de Etapa 2.

## Nota de implementacion

La regla operativa de esta frontera es simple:
- Etapa 2 define el lenguaje.
- Etapa 3 lo hace entrar al motor.

Si una decision no ayuda a que `gram.y`, `scan.l` y el dispatcher puedan implementarse sin ambiguedad, entonces esa decision aun no pertenece a Etapa 2.
