.timer on
CREATE SECRET fusion (TYPE oracle_fusion, PROVIDER browser, ENDPOINT '__FUSION_ENDPOINT__', REPORT_PATH '__FUSION_REPORT_PATH__');
SELECT signed_in FROM fusion_scanner_sso_login();
.print '=== NULL vs empty string'
SELECT n IS NULL AS n_is_null, length(n) AS n_len, e IS NULL AS e_is_null, y FROM oracle_fusion_query($$SELECT CAST(NULL AS VARCHAR2(10)) AS n, '' AS e, 'x' AS y FROM dual$$, secret := 'fusion', all_varchar := true);
.print '=== DATE with time: as DATE vs CAST AS TIMESTAMP vs TO_CHAR'
SELECT * FROM oracle_fusion_query($$SELECT SYSDATE AS d_raw, CAST(SYSDATE AS TIMESTAMP) AS d_ts, TO_CHAR(SYSDATE, 'YYYY-MM-DD HH24:MI:SS') AS d_char, creation_date, last_update_date FROM ap_invoices_all WHERE ROWNUM = 1$$, secret := 'fusion', all_varchar := true);
.print '=== throughput 10k fetch 5000'
CREATE TABLE t10k AS SELECT * FROM oracle_fusion_query($$SELECT invoice_id, line_number, line_type_lookup_code, amount, description, accounting_date, period_name, org_id, creation_date, last_update_date, quantity_invoiced, unit_price, po_header_id, base_amount FROM ap_invoice_lines_all ORDER BY invoice_id, line_number FETCH FIRST 10000 ROWS ONLY$$, secret := 'fusion', all_varchar := true, on_cast_error := 'error', fetch_size := 5000);
SELECT count(*) FROM t10k;
.print '=== throughput 100k fetch 5000'
CREATE TABLE t100k_5 AS SELECT * FROM oracle_fusion_query($$SELECT invoice_id, line_number, line_type_lookup_code, amount, description, accounting_date, period_name, org_id, creation_date, last_update_date, quantity_invoiced, unit_price, po_header_id, base_amount FROM ap_invoice_lines_all ORDER BY invoice_id, line_number FETCH FIRST 100000 ROWS ONLY$$, secret := 'fusion', all_varchar := true, on_cast_error := 'error', fetch_size := 5000);
SELECT count(*) FROM t100k_5;
.print '=== throughput 100k fetch 10000'
CREATE TABLE t100k_10 AS SELECT * FROM oracle_fusion_query($$SELECT invoice_id, line_number, line_type_lookup_code, amount, description, accounting_date, period_name, org_id, creation_date, last_update_date, quantity_invoiced, unit_price, po_header_id, base_amount FROM ap_invoice_lines_all ORDER BY invoice_id, line_number FETCH FIRST 100000 ROWS ONLY$$, secret := 'fusion', all_varchar := true, on_cast_error := 'error', fetch_size := 10000);
SELECT count(*) FROM t100k_10;
.print '=== throughput 500k fetch 10000'
CREATE TABLE t500k AS SELECT * FROM oracle_fusion_query($$SELECT invoice_id, line_number, line_type_lookup_code, amount, description, accounting_date, period_name, org_id, creation_date, last_update_date, quantity_invoiced, unit_price, po_header_id, base_amount FROM ap_invoice_lines_all ORDER BY invoice_id, line_number FETCH FIRST 500000 ROWS ONLY$$, secret := 'fusion', all_varchar := true, on_cast_error := 'error', fetch_size := 10000);
SELECT count(*) FROM t500k;
.print '=== typing step on 500k (contract inference, simplified)'
CREATE TABLE t500k_typed AS SELECT CAST(invoice_id AS VARCHAR) AS invoice_id, CAST(line_number AS BIGINT) AS line_number, line_type_lookup_code, CAST(amount AS DECIMAL(38,2)) AS amount, description, CAST(accounting_date AS DATE) AS accounting_date, period_name, CAST(org_id AS VARCHAR) AS org_id, CAST(creation_date AS TIMESTAMP) AS creation_date, CAST(last_update_date AS TIMESTAMP) AS last_update_date FROM t500k;
SELECT count(*), sum(amount) FROM t500k_typed;
