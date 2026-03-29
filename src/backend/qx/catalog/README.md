# QhapaqXian Catalog Layer

Planned responsibility:
- fork-owned helpers for agent metadata, namespace metadata, policy metadata,
  and runtime relation support.

Bootstrap note:
- true PostgreSQL catalog definitions still belong under `src/include/catalog/`
  and related upstream bootstrap machinery;
- this directory should hold fork-specific coordination logic, not duplicate
  the upstream catalog bootstrap mechanism.
