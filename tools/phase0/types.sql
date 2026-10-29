.timer on
CREATE SECRET fusion (TYPE oracle_fusion, PROVIDER browser, ENDPOINT '__FUSION_ENDPOINT__', REPORT_PATH '__FUSION_REPORT_PATH__');
SELECT host, subject, expires_at, signed_in FROM fusion_scanner_sso_login();
.print '=== A1/Q3/Q4 value formats (all_varchar)'
SELECT * FROM oracle_fusion_query($$SELECT 0.84 AS small_num, -1.5 AS neg, -0.5 AS neg_small, 300000012345678901 AS big_id, 12345678901234567890123 AS huge, DATE '2026-01-01' AS d, TIMESTAMP '2026-01-01 10:11:12.123456' AS ts, TRUNC(SYSDATE) AS td, SYSDATE AS sd, CAST(NULL AS VARCHAR2(10)) AS n, '0012' AS code, 'a''b <&>' AS esc, TO_CLOB('clob text') AS c, SYSTIMESTAMP AS tz, 1e30 AS big_float, BINARY_DOUBLE_INFINITY AS inf FROM dual$$, secret := 'fusion', all_varchar := true, on_cast_error := 'error');
.print '=== A2 odci collections'
SELECT * FROM oracle_fusion_query($$SELECT column_value AS v FROM TABLE(sys.odcinumberlist(1, 2, 3))$$, secret := 'fusion', all_varchar := true);
SELECT * FROM oracle_fusion_query($$SELECT column_value AS v FROM TABLE(sys.odcivarchar2list('a', 'b'))$$, secret := 'fusion', all_varchar := true);
SELECT * FROM oracle_fusion_query($$SELECT column_value AS v FROM TABLE(sys.odcidatelist(DATE '2026-01-01', TIMESTAMP '2026-01-02 03:04:05'))$$, secret := 'fusion', all_varchar := true);
SELECT * FROM oracle_fusion_query($$SELECT 1 AS x FROM dual WHERE 1 IN (SELECT column_value FROM TABLE(sys.odcinumberlist(1)) UNION ALL SELECT column_value FROM TABLE(sys.odcinumberlist(2)))$$, secret := 'fusion', all_varchar := true);
.print '=== 5.2 empty result'
SELECT * FROM oracle_fusion_query($$SELECT 1 AS x FROM dual WHERE 1 = 0$$, secret := 'fusion', all_varchar := true);
.print '=== volumes'
SELECT * FROM oracle_fusion_query($$SELECT COUNT(*) AS n_inv FROM ap_invoices_all$$, secret := 'fusion', all_varchar := true);
SELECT * FROM oracle_fusion_query($$SELECT COUNT(*) AS n_lines FROM ap_invoice_lines_all$$, secret := 'fusion', all_varchar := true);
SELECT * FROM oracle_fusion_query($$SELECT COUNT(*) AS n_dists FROM ap_invoice_distributions_all$$, secret := 'fusion', all_varchar := true);
