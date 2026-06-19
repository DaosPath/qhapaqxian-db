-- Stage 35 backend supervisor observability regression (no Docker required)

SELECT 42435::oid AS qx_sup_task_oid \gset

SELECT pg_qx_stat_reset('backend_supervisor');

CREATE EXTENSION dblink;

SELECT dblink_connect('qx_supervisor_peer',
                      'dbname=' || current_database());

SELECT dblink_exec('qx_supervisor_peer',
                   'DO $peer$ BEGIN PERFORM pg_qx_test_register_backend_lease(42435::oid, ''qx-container-test-1'', ''container''); END $peer$');

SELECT active_lease_count = 1
   AND register_count = 1 AS supervisor_stats_after_cross_process_register
FROM pg_stat_qx_backend_supervisor;

SELECT count(*) = 1
   AND bool_and(lease_kind = 'container') AS supervisor_list_after_cross_process_register
FROM pg_qx_backend_supervisor_list() AS l(task_oid oid,
                                           instance_id text,
                                           lease_kind text);

SELECT pg_qx_test_release_backend_lease(:qx_sup_task_oid, 'qx-container-test-1');

SELECT active_lease_count = 0
   AND release_count = 1 AS supervisor_stats_after_release
FROM pg_stat_qx_backend_supervisor;

SELECT pg_qx_test_register_backend_lease(:qx_sup_task_oid,
                                           'qx-microvm-test-1',
                                           'microvm');

SELECT pg_qx_test_fence_backend_leases(:qx_sup_task_oid);

SELECT active_lease_count = 0
   AND fence_count = 1 AS supervisor_stats_after_fence
FROM pg_stat_qx_backend_supervisor;

SELECT dblink_disconnect('qx_supervisor_peer');

DROP EXTENSION dblink;
