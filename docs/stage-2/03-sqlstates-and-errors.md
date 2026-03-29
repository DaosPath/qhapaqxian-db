# QhapaqXian DB Etapa 2 - Politica de SQLSTATEs y Errores

Estado: borrador controlado para la especificacion del lenguaje agentic v0.

Alcance:
- aplica a los comandos de Etapa 2 del lenguaje nativo de QhapaqXian DB;
- define contratos de error publicos antes de implementar el parser;
- separa errores de usuario, errores de policy y fallas internas del motor.

Reglas base:
- un error de comando debe producir `ERROR` por defecto;
- `WARNING` y `NOTICE` quedan reservados para casos no bloqueantes;
- `FATAL` y `PANIC` no son parte del contrato de Etapa 2;
- `XX000` se reserva para invariantes internas o fallas no clasificadas.

## 1. Criterios de diseno

### Principios
1. Cada comando de Etapa 2 debe tener errores predecibles, con SQLSTATE estable.
2. Los errores de validacion de usuario deben preferir codigos especificos antes que `XX000`.
3. Los errores de policy y presupuesto deben diferenciarse de los errores sintacticos.
4. Los errores de conflicto de estado deben ser distinguibles de permisos y de validacion semantica.
5. Los mensajes deben ser accionables, sin exponer internals innecesarios.

### Jerarquia de falla
- `syntax/parse` -> `42601` o error propio de entrada invalida.
- `semantic validation` -> codigo QhapaqXian propio o reutilizacion de `22023`.
- `authz/policy` -> `42501` o `QX1xx`.
- `state conflict` -> `55000` o `QX2xx`.
- `runtime/budget/recovery` -> `57014`, `57P01`, `QX3xx`.
- `internal invariant` -> `XX000`.

### Politica de estabilidad publica
- el SQLSTATE es el identificador contractual;
- el texto de `MESSAGE` puede refinarse sin romper compatibilidad;
- `DETAIL` y `HINT` pueden ampliarse mientras el SQLSTATE y la causa semantica no cambien;
- una vez liberada la Etapa 3, no se debe reciclar un SQLSTATE para una causa distinta;
- si una clase de error nueva aparece, se asigna un SQLSTATE nuevo antes de mover el parser a produccion.

## 2. Estrategia de SQLSTATE

QhapaqXian DB puede usar codigos propios sin chocar con PostgreSQL si reserva una familia estable para el dominio agentic.

### Propuesta de familia propia
Usar codigos `QX###` de cinco caracteres, donde:
- `QX` identifica el dominio agentic de QhapaqXian DB;
- los ultimos tres caracteres distinguen la causa.

### Asignacion inicial
| SQLSTATE | Clase | Uso |
|---|---|---|
| `QX000` | Internal generic | fallo interno del subsistema agentic no clasificado |
| `QX001` | Agent not found | agente inexistente |
| `QX002` | Agent identity missing | identidad inexistente o invalida |
| `QX003` | Agent policy violation | acceso o accion denegada por policy agentic |
| `QX004` | Agent budget exceeded | presupuesto agotado |
| `QX005` | Agent state invalid | comando incompatible con el estado actual |
| `QX006` | Session conflict | conflicto de sesion o lease |
| `QX007` | Task conflict | tarea ya existe, ya corre o no admite la transicion |
| `QX008` | Memory error | scope, tipo o payload de memoria invalido |
| `QX009` | Checkpoint error | checkpoint invalido o no resumible |
| `QX010` | Tool not allowed | herramienta no autorizada |
| `QX011` | Namespace violation | agente fuera de namespace/policy |
| `QX012` | Trace unavailable | trazas no disponibles o no habilitadas |
| `QX013` | Explain invalid | `EXPLAIN AGENT` sobre sentencia no soportada |

### Reutilizacion de codigos PostgreSQL
Usar codigos existentes cuando la causa ya esta bien cubierta:
- `42601` para errores de sintaxis y parsing;
- `42703` para nombres inexistentes cuando la semantica sea la de identificador SQL normal;
- `42501` para denegacion clasica de privilegios SQL;
- `22023` para parametro invalido o valor invalido;
- `23505` para duplicados o violacion de unicidad;
- `55000` para estado de objeto no valido;
- `57014` para cancelacion o timeout;
- `XX000` para invariantes internas.

La regla es simple:
- si el error pertenece al dominio SQL clasico, reutilizar PostgreSQL;
- si el error pertenece al dominio agentic y necesita semantica propia, usar `QX###`.

## 3. Catalogo inicial por comando

### `CREATE AGENT`
| Condicion | SQLSTATE | Severidad | MESSAGE | DETAIL | HINT |
|---|---|---|---|---|---|
| Sintaxis invalida | `42601` | `ERROR` | invalid agent definition syntax | la sentencia no cumple la gramatica de `CREATE AGENT` | revise el orden de clausulas y la puntuacion |
| Agente duplicado | `23505` | `ERROR` | agent already exists | el nombre del agente ya existe en el namespace actual | use otro nombre o `IF NOT EXISTS` cuando exista |
| Identidad inexistente | `QX002` | `ERROR` | agent identity does not exist | la identidad referenciada no esta registrada | cree la identidad antes de crear el agente |
| Policy inexistente | `QX003` | `ERROR` | agent policy is not allowed or not found | la policy no existe o no es visible para el rol actual | verifique ownership y permisos de policy |
| Budget invalido | `22023` | `ERROR` | invalid agent budget specification | un limite de tiempo, costo o tokens es inconsistente | corrija los limites y use valores positivos |

### `ALTER AGENT`
| Condicion | SQLSTATE | Severidad | MESSAGE | DETAIL | HINT |
|---|---|---|---|---|---|
| Agente inexistente | `QX001` | `ERROR` | agent does not exist | no hay un agente con ese nombre | confirme el namespace o cree el agente primero |
| Transicion no permitida | `QX005` | `ERROR` | agent state does not allow this alteration | la alteracion no es valida en el estado actual | espere a que termine la tarea o reanude desde un checkpoint |
| Tool no autorizada | `QX010` | `ERROR` | tool is not allowed for this agent | la tool no esta en la policy o en el allowlist | agregue la tool a la policy correcta |
| Cambio de identidad bloqueado | `QX003` | `ERROR` | agent identity change is not allowed | la identidad activa no puede cambiarse sin privilegio | use un flujo administrativo o cree un agente nuevo |

### `START SESSION`
| Condicion | SQLSTATE | Severidad | MESSAGE | DETAIL | HINT |
|---|---|---|---|---|---|
| Agente inexistente | `QX001` | `ERROR` | agent does not exist | no se encontro el agente solicitado | verifique el nombre completo del agente |
| Policy denegada | `QX003` | `ERROR` | session is not allowed by policy | el rol actual no puede iniciar sesiones para ese agente | pida acceso al namespace o cambie la policy |
| Namespace invalido | `QX011` | `ERROR` | agent namespace violation | el agente no pertenece al namespace visible | revise ownership, schema o tenant |
| Sesion ya activa | `QX006` | `ERROR` | active session already exists | existe una sesion activa incompatible con la nueva | cierre o reanude la sesion previa |

### `RUN TASK`
| Condicion | SQLSTATE | Severidad | MESSAGE | DETAIL | HINT |
|---|---|---|---|---|---|
| Sesion inexistente | `QX006` | `ERROR` | session does not exist | la sesion indicada no existe | verifique el `SESSION` objetivo |
| Sesion cerrada | `QX005` | `ERROR` | task cannot run on this session state | la sesion no esta abierta para ejecucion | inicie una nueva sesion o reanude una valida |
| Presupuesto agotado | `QX004` | `ERROR` | agent budget exceeded | no hay presupuesto para lanzar la tarea | incremente el budget o reduzca el goal |
| Goal vacio o invalido | `22023` | `ERROR` | invalid task goal | el objetivo de la tarea no es utilizable | defina un `GOAL` concreto |
| Tool no autorizada | `QX010` | `ERROR` | task requires a tool that is not allowed | la tarea quiere usar una herramienta bloqueada | ajuste policy o tools del agente |

### `REMEMBER`
| Condicion | SQLSTATE | Severidad | MESSAGE | DETAIL | HINT |
|---|---|---|---|---|---|
| Sesion invalida | `QX006` | `ERROR` | session is not valid for memory write | la sesion no esta activa o no pertenece al agente | use una sesion abierta |
| Scope invalido | `QX008` | `ERROR` | invalid memory scope | el scope no existe o no aplica a esta operacion | use `WORKING`, `EPISODIC` o `SEMANTIC` segun corresponda |
| Payload no serializable | `22023` | `ERROR` | invalid memory payload | el valor no puede persistirse de forma estable | serialice el payload como `jsonb` o formato soportado |
| Namespace violation | `QX011` | `ERROR` | memory write violates namespace policy | la memoria cruza un limite de aislamiento | escriba dentro del namespace correcto |

### `FETCH MEMORY`
| Condicion | SQLSTATE | Severidad | MESSAGE | DETAIL | HINT |
|---|---|---|---|---|---|
| Agente inexistente | `QX001` | `ERROR` | agent does not exist | no hay agente para recuperar memoria | verifique el nombre del agente |
| Scope invalido | `QX008` | `ERROR` | invalid memory scope | el scope solicitado no esta soportado | revise los scopes disponibles |
| Sin datos | `02000` | `NOTICE` o `ERROR` segun clausula | no matching memory found | la consulta no encontro memoria relevante | amplie `MATCH` o cambie el `LIMIT` |
| Recuperacion deshabilitada | `QX012` | `ERROR` | memory retrieval is unavailable | la funcionalidad no esta habilitada o indexada aun | habilite el subsistema o use un modo de degradacion |

### `CHECKPOINT TASK`
| Condicion | SQLSTATE | Severidad | MESSAGE | DETAIL | HINT |
|---|---|---|---|---|---|
| Tarea inexistente | `QX007` | `ERROR` | task does not exist | la tarea objetivo no existe | verifique el id de tarea |
| Tarea no checkpointable | `QX005` | `ERROR` | task cannot be checkpointed in current state | la tarea no admite checkpoint en este punto | espere a un safe point valido |
| Checkpoint duplicado | `23505` | `ERROR` | checkpoint already exists | ya existe un checkpoint con esa etiqueta | use otra etiqueta o reanude desde el existente |

### `RESUME TASK`
| Condicion | SQLSTATE | Severidad | MESSAGE | DETAIL | HINT |
|---|---|---|---|---|---|
| Tarea inexistente | `QX007` | `ERROR` | task does not exist | la tarea objetivo no existe | verifique el id de tarea |
| Checkpoint invalido | `QX009` | `ERROR` | checkpoint is not resumable | el checkpoint no puede usarse para reanudar | elija un checkpoint compatible |
| Conflicto de lease | `QX006` | `ERROR` | task is already owned by another active lease | hay una ejecucion viva o huella reciente incompatible | espere a que expire el lease o cancele la ejecucion anterior |
| Estado incompatible | `QX005` | `ERROR` | task state does not allow resume | la tarea ya termino o quedo en un estado terminal | cree una nueva tarea o revise el ultimo checkpoint |

### `SHOW TRACE`
| Condicion | SQLSTATE | Severidad | MESSAGE | DETAIL | HINT |
|---|---|---|---|---|---|
| Tarea inexistente | `QX007` | `ERROR` | task does not exist | no existe la tarea pedida | verifique el id |
| Trazas deshabilitadas | `QX012` | `WARNING` o `ERROR` | trace data is unavailable | la persistencia de trazas no esta activa | habilite tracing para la sesion o tarea |
| Limite invalido | `22023` | `ERROR` | invalid trace limit | `LIMIT` no es valido | use un entero positivo |

### `EXPLAIN AGENT`
| Condicion | SQLSTATE | Severidad | MESSAGE | DETAIL | HINT |
|---|---|---|---|---|---|
| Sentencia no agentic | `QX013` | `ERROR` | statement is not explainable as an agent command | `EXPLAIN AGENT` requiere una sentencia agentic valida | use `EXPLAIN AGENT` con `RUN TASK`, `START SESSION` o una sentencia soportada |
| Agente inexistente | `QX001` | `ERROR` | agent does not exist | el agente referenciado no existe | cree el agente o corrija el nombre |

## 4. Severidad y formato de respuesta

### Severidad recomendada
- `ERROR` para toda violacion funcional de Etapa 2;
- `NOTICE` solo para ausencia de resultados cuando el comando lo tolere;
- `WARNING` solo para degradacion no fatal del plan o de la trazabilidad;
- `FATAL` y `PANIC` quedan fuera del contrato de usuario.

### Formato de mensaje
Cada error debe producir, cuando aplique:
- `MESSAGE`: resumen corto y estable;
- `DETAIL`: explicacion concreta del estado o condicion;
- `HINT`: accion siguiente sugerida;
- `CONTEXT`: solo si ayuda a ubicar la sentencia o el subpaso.

Regla de redaccion:
- `MESSAGE` no debe cambiar de significado entre releases menores;
- `DETAIL` puede volverse mas preciso;
- `HINT` puede mejorar con nuevas capacidades del motor.

## 5. Criterios de estabilidad publica

Una vez que Etapa 3 empiece a aceptar parser real, la politica de errores pasa a ser contrato publico.

### Invariantes
1. Un SQLSTATE asignado no cambia de significado.
2. Un SQLSTATE no se reutiliza para otro comando o causa.
3. Un error de policy no se degrada silenciosamente a `XX000`.
4. Un error de estado no se convierte en error de sintaxis.
5. Un mensaje puede refinarse, pero el tipo de fallo debe permanecer equivalente.

### Lo que si puede evolucionar
- texto exacto del `MESSAGE`;
- redaccion de `DETAIL`;
- sugerencias de `HINT`;
- clasificacion entre `NOTICE` y `WARNING` en casos no bloqueantes.

### Criterio de congelamiento
La interfaz de errores de Etapa 2 se considera estable cuando:
- el lenguaje agentic tiene su sintaxis base definida;
- cada comando principal tiene al menos un SQLSTATE propio o reutilizado documentado;
- la matriz de errores esta cubierta por pruebas de parser y semantic validation;
- el documento de politica no requiere cambios estructurales para pasar a Etapa 3.

## 6. Nota de implementacion para Etapa 3

Este documento no asume aun cambios en `gram.y` o en los handlers del servidor.
Su objetivo es fijar la semantica de error antes de codificar el parser.

Orden recomendado para la implementacion posterior:
1. registrar los SQLSTATEs en el analizador;
2. mapear cada comando a errores predecibles;
3. agregar tests negativos por comando;
4. congelar `MESSAGE`, `DETAIL` y `HINT` antes de abrir Etapa 3.
