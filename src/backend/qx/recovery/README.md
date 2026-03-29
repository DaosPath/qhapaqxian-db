# QhapaqXian Recovery Layer

Planned responsibility:
- scan durable task state after restart
- fence stale leases
- classify orphaned attempts
- requeue resumable tasks

Bootstrap note:
- initial recovery logic should reconstruct runtime state from ordinary logged
  relations before any dedicated WAL families are introduced.
