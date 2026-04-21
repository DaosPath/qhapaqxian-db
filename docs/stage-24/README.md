# Stage 24: Container Backend Scaffold

Stage 24 introduces the first container-backend contract for QhapaqXian DB.
It landed as a scaffold before the later real Docker-backed integration.

What landed:
- `src/backend/qx/runtime/qx_container_backend.h`
- `src/backend/qx/runtime/qx_container_backend.c`
- runtime-module build wiring for the new backend scaffold

Backend contract:
- request validation is explicit and rejects mismatched runtime classes;
- the backend only accepts `provider kind = container`;
- the backend only accepts `principal runtime = container`;
- the backend only accepts `container://` endpoints;
- the backend requires `isolated` sandboxing;
- the backend requires `qx.receipt.v1` receipt shape and `ed25519` when attestation is required;
- request payloads are serialized as line-oriented `key=value` contracts so `qx_runtime.c` can later hand them to a real broker or container provider;
- broker responses are parsed back into a typed response struct with launch/verification booleans, runtime identity, receipt details, and broker-observed execution data.

What is wired:
- request and response structs;
- init/free helpers;
- validation helpers for provider kind, runtime class, endpoint prefix, sandbox, and receipt expectations;
- launch-request serialization;
- launch-response parsing.

What was deferred at landing time:
- real container launch;
- OCI spec generation;
- cgroup / namespace / seccomp enforcement;
- rootfs or image resolution;
- transport from the backend to a real container engine;
- integration into `qx_runtime.c`.

How it integrates now:
- `qx_runtime.c` builds a `QxContainerBackendRequest` from the already-authorized runtime contract;
- the runtime calls `qx_container_backend_validate_request()` before backend handoff;
- the runtime uses `qx_container_backend_build_launch_request()` to produce the runner payload;
- the runner launches Docker and returns backend evidence;
- the runtime parses the backend response with `qx_container_backend_parse_launch_response()`;
- the runtime calls `qx_container_backend_validate_response()` before charging budget or emitting final traces.

Known gaps at landing time:
- this stage did not claim container execution was real by itself;
- the backend assumed a brokered launch plane;
- the scaffold was intentionally shaped for later replacement of the broker
  with a real container runtime.

Post-stage integration note:
- a later integration pass wired this contract into `qx_runtime.c` as a real
  Docker-backed execution path;
- `QX_CONTAINER_IMAGE` selects the image used by the backend;
- `QX_DOCKER_CLI` can point to a concrete Docker client when `docker` is not on
  `PATH`;
- WSL may call Docker Desktop through Windows `docker.exe` and
  `DOCKER_HOST=npipe:////./pipe/dockerDesktopLinuxEngine`;
- the remaining gap is not "fake container launch" anymore, but deeper OCI
  ownership such as cgroups, seccomp, image policy, rootless mode, and richer
  container lifecycle supervision.
- operational setup and validation commands live in
  `../real-runtime-backends.md`.
