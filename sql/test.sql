DROP EXTENSION IF EXISTS dynamic_ext CASCADE;
CREATE EXTENSION dynamic_ext;
DROP TABLE IF EXISTS t;
CREATE TABLE t (k int4);
INSERT INTO t SELECT generate_series(1, 5000);
CREATE INDEX t_de ON t USING dynamic_ext (k);
SHOW data_directory;
SELECT pg_relation_filenode('t_de');
SELECT oid FROM pg_database WHERE datname = current_database();
SELECT reltuples FROM pg_class WHERE relname = 't_de';


DROP TABLE IF EXISTS u;
CREATE UNLOGGED TABLE u (k int4);
INSERT INTO u SELECT generate_series(1, 5000);
CREATE INDEX u_de ON u USING dynamic_ext (k);
SELECT pg_relation_filenode('u_de');