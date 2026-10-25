\echo Use "CREATE EXTENSION dynamic_ext" to load this file. \quit

CREATE FUNCTION dynamic_ext(internal)
RETURNS index_am_handler
AS 'MODULE_PATHNAME', 'dynamic_ext'
LANGUAGE C;

CREATE ACCESS METHOD dynamic_ext TYPE INDEX HANDLER dynamic_ext;

CREATE OPERATOR CLASS int4_ops
    DEFAULT FOR TYPE int4 USING dynamic_ext AS
        OPERATOR 1 <  ,
        OPERATOR 2 <= ,
        OPERATOR 3 =  ,
        OPERATOR 4 >= ,
        OPERATOR 5 >  ,
        FUNCTION 1 btint4cmp(int4, int4);