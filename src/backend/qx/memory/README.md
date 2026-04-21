# QhapaqXian Memory Layer

Role:
- manage working, episodic, and semantic memory snapshots plus checkpoint
  payload shaping.

Current state:
- memory data still lives on ordinary PostgreSQL relations and helper code.

Integration debt:
- deeper storage methods, specialized indexes, and benchmark-driven layout
  changes are still deferred.
