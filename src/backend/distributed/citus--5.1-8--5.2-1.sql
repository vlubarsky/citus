/* citus--5.1-8--5.2-1.sql */

/* empty, but required to update the extension version */

CREATE SEQUENCE citus.pg_dist_colocation_id
    MINVALUE 1
    NO CYCLE;
ALTER SEQUENCE  citus.pg_dist_colocation_id SET SCHEMA pg_catalog;

ALTER TABLE pg_dist_partition ADD COLUMN colocationId BIGINT DEFAULT nextval('pg_dist_colocation_id') ;

/* a very simple UDF that only sets the colocation ids the same */
CREATE OR REPLACE FUNCTION colocate_tables(source_table regclass, target_table regclass)
    RETURNS BOOL
    LANGUAGE plpgsql
    AS $colocate_tables$
DECLARE nextid BIGINT;
BEGIN
	SELECT nextval('pg_dist_colocation_id') INTO nextid;

	UPDATE pg_dist_partition SET colocationId = nextid
    WHERE logicalrelid IN
	(
		(SELECT p1.logicalrelid
			FROM pg_dist_partition p1, pg_dist_partition p2
			WHERE
				p2.logicalrelid = source_table AND
				p1.colocationId = p2.colocationId)
		UNION
		(SELECT target_table)
	);
	RETURN TRUE;
END;
$colocate_tables$ SET search_path = 'pg_catalog';

