# QhapaqXian Engine Public Backend Headers

This directory is reserved for fork-owned headers shared across backend
subsystems.

Expected future headers:
- runtime state
- scheduler APIs
- recovery primitives
- budget and policy structs
- explain/tracing structs

Bootstrap note:
- parser node definitions and catalog bootstrap definitions still belong in
  standard PostgreSQL include locations until their upstream integration points
  are introduced.
