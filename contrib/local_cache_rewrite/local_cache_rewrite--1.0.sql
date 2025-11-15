-- local_cache_rewrite extension
CREATE FUNCTION local_cache_rewrite_init() RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C;
