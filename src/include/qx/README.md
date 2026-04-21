# QhapaqXian Public Backend Headers

This tree holds fork-owned headers shared across backend subsystems. Keep these
headers small, stable, and focused on cross-module data contracts.

Current public surfaces:
- catalog snapshots and lookup/free APIs
- runtime, scheduler, recovery, and observability structs as they land
- policy, budget, and identity contracts shared by security and runtime

Boundary rule:
- parser node definitions and catalog bootstrap definitions stay in the normal
  PostgreSQL include paths until the fork adds a true upstream replacement.
