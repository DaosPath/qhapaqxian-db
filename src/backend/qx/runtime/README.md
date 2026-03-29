# QhapaqXian Runtime Layer

Planned responsibility:
- launcher
- scheduler
- dispatcher
- executor workers
- recovery scanner
- budget keeper

Bootstrap rule:
- the first runtime implementation will likely sit on top of PostgreSQL
  background workers and shared memory;
- this directory marks the stable ownership boundary for that work.
