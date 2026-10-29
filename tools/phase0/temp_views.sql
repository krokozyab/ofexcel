CREATE SCHEMA ds;
CREATE TABLE ds."ap_invoices__abc__v1" AS SELECT 1 AS invoice_id, 100.5 AS amount;
CREATE TABLE ap_invoices AS SELECT 999 AS invoice_id, 0 AS amount;
CREATE TEMP VIEW ap_invoices AS SELECT * FROM ds."ap_invoices__abc__v1";
.print '--- short name with a same-named main table present (expect 1 from the temp view)'
SELECT invoice_id FROM ap_invoices;
.print '--- prepared statement with named parameter'
PREPARE q AS SELECT invoice_id FROM ap_invoices WHERE amount > $min_amount;
EXECUTE q(min_amount := 50);
DROP VIEW temp.ap_invoices;
.print '--- after dropping the temp view (expect 999)'
SELECT invoice_id FROM ap_invoices;
