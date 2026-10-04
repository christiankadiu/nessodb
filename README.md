# NessoDB


NessoDB is an educational relational database engine written in C++23. It
supports SQL parsing and binding, logical and physical plans, query execution,
paged storage, B+ tree primary-key indexes, transactions, and write-ahead-log
recovery.

## Highlights

- Hand-written lexer and recursive-descent SQL parser with source locations.
- Semantic binding against an in-memory catalog.
- Logical and physical plans with human-readable `EXPLAIN` output.
- Pull-based, batched execution operators for scans, filters, projections,
  joins, sorting, distinct results, limits, and aggregation.
- Persistent 4 KiB pages with versioned headers, reserved-field validation,
  and CRC32C checksums.
- Buffer pool, slotted heap pages, versioned row encoding, and persistent
  catalog metadata.
- Persistent B+ tree primary-key indexes with splits, redistribution, merging,
  point lookup, and range scans.
- Explicit and autocommit transactions, table-level shared/exclusive locks,
  undo logging, write-ahead logging, and restart recovery.
- No runtime dependencies outside the C++ standard library.

## Build and test

Requirements:

- CMake 3.21 or newer;
- a compiler with the C++23 standard library features used by the project
  (including `std::expected`);
- a platform with CMake thread support for the concurrency tests.

```sh
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Debug \
  -DNESSODB_BUILD_TESTS=ON
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

Omit `-DNESSODB_BUILD_TESTS=ON` when only the library and CLI are needed.
Strict warnings are enabled for GCC, Clang, and MSVC builds.

## Try it

The CLI accepts a database path followed by one or more SQL statements. Use
`:memory:` for an ephemeral database:

```sh
./build/nessodb :memory: \
  "CREATE TABLE users(id INT PRIMARY KEY, name TEXT);" \
  "INSERT INTO users VALUES (1, 'Alice');" \
  "INSERT INTO users VALUES (2, 'Bob');" \
  "SELECT name, id + 100 AS score FROM users ORDER BY id;"
```

Expected output:

```text
Alice | 101
Bob | 102
```

Pass a file instead to create or reopen a persistent database:

```sh
./build/nessodb example.ndb \
  "CREATE TABLE events(id INT PRIMARY KEY, label TEXT);" \
  "INSERT INTO events VALUES (1, 'started');"

./build/nessodb example.ndb "SELECT * FROM events;"
```

The CLI executes each argument as one statement and does not provide an
interactive shell.

## SQL surface

The implemented subset includes:

- `CREATE TABLE` with `INT`, `TEXT`, and single-column `PRIMARY KEY`;
- `INSERT`, `UPDATE`, and `DELETE`;
- `SELECT`, aliases, qualified columns, and arithmetic expressions;
- comparisons, `IS NULL`, `IS NOT NULL`, `AND`, `OR`, and `NOT`;
- inner `JOIN ... ON`;
- `DISTINCT`, `ORDER BY`, `LIMIT`, and `OFFSET`;
- `COUNT`, `MIN`, `MAX`, `SUM`, and `GROUP BY`;
- `EXPLAIN SELECT ...`;
- `BEGIN`, `COMMIT`, and `ROLLBACK`.

The SQL grammar does not include subqueries, outer joins, schema alteration,
user-created secondary indexes, floating-point or date types, or a cost-based
optimizer. DDL is not allowed inside an explicit transaction.

## Architecture

```text
SQL text
   |
   v
lexer -> parser -> AST -> binder + catalog
                            |
                            v
                logical plan -> physical plan
                                    |
                                    v
                           execution operators
                                    |
                    +---------------+---------------+
                    |                               |
              in-memory heap                 storage manager
                                                    |
                         +--------------------------+----------+
                         |                          |          |
                    table heaps                 B+ trees   catalog
                         |                          |
                         +------------+-------------+
                                      |
                                  buffer pool
                                      |
                            database file + WAL
```

The SQL frontend does not depend on storage, and storage does not interpret
SQL. `engine::Database` coordinates the layers and selects either ephemeral or
persistent storage. Execution uses bounded row batches, although table scans
and final query results are still materialized at the engine boundary.

The source tree mirrors these boundaries:

```text
apps/cli/         command-line driver
src/sql/          lexer, parser, and AST
src/binder/       semantic analysis and bound expressions
src/catalog/      logical schemas and table identity
src/planner/      logical/physical plans and plan formatting
src/execution/    physical operators and expression evaluation
src/index/        key encoding and persistent B+ tree
src/storage/      pages, records, buffer pool, heaps, and catalog store
src/transaction/  transaction state and table locks
src/recovery/     write-ahead log and restart recovery
src/engine/       public database coordinator and query results
tests/            unit, integration, persistence, and recovery tests
```

The binary page layout and corruption checks are documented in
[docs/storage/page-format.md](docs/storage/page-format.md).

## Current limitations

- One `Database` instance supports one active transaction; it is not a
  multi-client database server.
- Concurrency control uses table-level locks rather than MVCC or row locks.
- Sort, distinct, joins, and aggregation are memory-resident and do not spill
  to disk.
- Physical planning recognizes primary-key equality lookups, but there is no
  general-purpose optimizer, statistics system, or cost model.
- Storage appends new pages and does not yet reclaim them through the reserved
  free-list metadata or provide a vacuum operation.
- The C++ headers under `src/` are not yet a stable installed library API.

## License

NessoDB is available under the [MIT License](LICENSE).
