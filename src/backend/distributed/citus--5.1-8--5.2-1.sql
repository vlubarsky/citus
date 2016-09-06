/* citus--5.1-8--5.2-1.sql */

/* empty, but required to update the extension version */

DROP FUNCTION IF EXISTS pg_catalog.worker_fetch_regular_table(text, text, bigint, text[], integer[]);

CREATE OR REPLACE FUNCTION worker_fetch_regular_table(text, bigint, text[], integer[], bool DEFAULT TRUE)
    RETURNS void
    LANGUAGE C STRICT
    AS 'citus', $$worker_fetch_regular_table$$;
COMMENT ON FUNCTION worker_fetch_regular_table(text, bigint, text[], integer[])
    IS 'fetch PostgreSQL table from remote node';