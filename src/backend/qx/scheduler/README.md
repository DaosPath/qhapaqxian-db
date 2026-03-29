# QhapaqXian Scheduler Layer

Planned responsibility:
- task admission
- priority calculation
- fairness across namespaces and agents
- retry scheduling
- worker assignment policy

Bootstrap note:
- the scheduler should start as a background-worker-based subsystem before
  deeper integration with planner and recovery semantics.
