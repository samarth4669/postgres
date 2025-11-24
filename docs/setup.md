
```sql
CREATE EXTENSION IF NOT EXISTS postgres_fdw;
```

```sql
CREATE SERVER foreign_pg_server
FOREIGN DATA WRAPPER postgres_fdw
OPTIONS (host '10.129.131.148', port '5432', dbname 'mydatabase');


CREATE USER MAPPING FOR current_user
SERVER foreign_pg_server
OPTIONS (user 'admin', password 'admin123');


IMPORT FOREIGN SCHEMA public
FROM SERVER foreign_pg_server
INTO public;
```
