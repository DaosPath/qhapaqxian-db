# QhapaqXian DB Stage README Template

Use this layout for stage READMEs so each stage tells the same story in the
same order.

Recommended section order:
- Stage intent
- What landed
- What is intentionally deferred
- Why this stage matters
- Validation
- Files touched
- Next stage handoff

Style rules:
- keep the write-up honest about what is actually wired;
- separate docs-only work from engine behavior;
- call out deferred runtime integration explicitly when the stage is a bridge;
- keep validation short, reproducible, and tied to the stage scope.

This template is the target shape for new stage docs and for future cleanup
passes on older stages.
