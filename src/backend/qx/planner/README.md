# QhapaqXian Planner Layer

Role:
- construct `AgentPlan` objects, cost agent steps, and decide retry / branch /
  replan shape.

Current state:
- planner boundary exists and feeds the current task execution path.

Integration debt:
- cost and branching logic are still narrower than the target agent-aware
  optimizer;
- some execution contracts still arrive from utility and security snapshots.
