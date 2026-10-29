CREATE SECRET fusion (TYPE oracle_fusion, PROVIDER browser, ENDPOINT '__FUSION_ENDPOINT__', REPORT_PATH '__FUSION_REPORT_PATH__');
SET allow_community_extensions = false;
SET enable_external_access = false;
SET lock_configuration = true;
SELECT signed_in FROM fusion_scanner_sso_login();
.print '--- query with external access off and configuration locked'
SELECT * FROM oracle_fusion_query($$SELECT 1 AS x FROM dual$$, secret := 'fusion', all_varchar := true);
SELECT * FROM oracle_fusion_tables() LIMIT 2;
.print '--- local file read must be refused'
SELECT * FROM read_csv('/etc/hosts');
