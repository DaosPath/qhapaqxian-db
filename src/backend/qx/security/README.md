# QhapaqXian Security Layer

Role:
- map SQL roles to agent principals, enforce namespaces, check tool ACLs, and
  evaluate session/task policy.

Current state:
- security owns the current identity and tool authorization path.

Integration debt:
- caller migration to the new catalog helper layer is still incomplete;
- remote provider trust and stronger isolation models remain iterative.
