-- Loads 500,000 real AP invoice lines into probe.ap_lines for the add-in probe.
-- Run through start.sh, which fills in the instance and signs in through SSO.
CREATE SECRET fusion (TYPE oracle_fusion, PROVIDER browser, ENDPOINT '__FUSION_ENDPOINT__', REPORT_PATH '__FUSION_REPORT_PATH__');
SELECT * FROM fusion_scanner_sso_login();
CREATE SCHEMA IF NOT EXISTS probe;
CREATE OR REPLACE TABLE probe.ap_lines_raw AS
SELECT * FROM oracle_fusion_query('SELECT invoice_id, line_number, line_type_lookup_code, amount, description,
           accounting_date, period_name, org_id, creation_date, last_update_date, quantity_invoiced,
           unit_price, po_header_id, base_amount
      FROM ap_invoice_lines_all
     ORDER BY invoice_id, line_number
     FETCH FIRST 500000 ROWS ONLY',
    secret := 'fusion', all_varchar := true, on_cast_error := 'error', fetch_size := 10000);
-- Typed the way CONTRACTS.md §3.3 would type it: '' is NULL, identifiers stay text.
CREATE OR REPLACE TABLE probe.ap_lines AS
SELECT NULLIF(INVOICE_ID, '') AS INVOICE_ID,
       TRY_CAST(NULLIF(LINE_NUMBER, '') AS BIGINT) AS LINE_NUMBER,
       NULLIF(LINE_TYPE_LOOKUP_CODE, '') AS LINE_TYPE_LOOKUP_CODE,
       TRY_CAST(NULLIF(AMOUNT, '') AS DECIMAL(38, 2)) AS AMOUNT,
       NULLIF(DESCRIPTION, '') AS DESCRIPTION,
       TRY_CAST(NULLIF(ACCOUNTING_DATE, '') AS DATE) AS ACCOUNTING_DATE,
       NULLIF(PERIOD_NAME, '') AS PERIOD_NAME,
       NULLIF(ORG_ID, '') AS ORG_ID,
       TRY_CAST(NULLIF(CREATION_DATE, '') AS TIMESTAMP) AS CREATION_DATE,
       TRY_CAST(NULLIF(LAST_UPDATE_DATE, '') AS TIMESTAMP) AS LAST_UPDATE_DATE,
       TRY_CAST(NULLIF(QUANTITY_INVOICED, '') AS DOUBLE) AS QUANTITY_INVOICED,
       TRY_CAST(NULLIF(UNIT_PRICE, '') AS DOUBLE) AS UNIT_PRICE,
       NULLIF(PO_HEADER_ID, '') AS PO_HEADER_ID,
       TRY_CAST(NULLIF(BASE_AMOUNT, '') AS DECIMAL(38, 2)) AS BASE_AMOUNT
  FROM probe.ap_lines_raw;
DROP TABLE probe.ap_lines_raw;
