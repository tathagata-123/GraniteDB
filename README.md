# SimpleDB

SimpleDB is a PostgreSQL-inspired database engine written in C++.

The project focuses on core DBMS internals: slotted-page storage, B+ tree indexing, a buffer pool, physical query execution, cost-based optimization, strict two-phase locking, and WAL-based crash recovery.

## Highlights

- **Storage and indexing**
  - slotted-page heap files
  - free-space tracking
  - buffer pool with clock replacement
  - single-column and composite-key B+ tree indexes
  - coordinated heap/index maintenance through the table layer

- **Query processing**
  - sequential scan
  - index scan
  - bitmap index scan and bitmap heap scan
  - nested-loop join, index nested-loop join, memoized index nested-loop join
  - hash join and merge join
  - sort, projection, filter, aggregation, set operations, top-N, and limit

- **Query optimization**
  - Selinger-style dynamic programming enumeration
  - startup-cost vs total-cost tracking
  - histograms, MCVs, and pairwise extended statistics
  - order-aware path retention
  - bitmap AND/OR planning

- **Transactions and concurrency**
  - strict 2PL
  - multiple-granularity locking
  - deadlock detection
  - key-range locking for phantom prevention on indexed scans

- **Recovery**
  - write-ahead logging
  - checkpoints
  - analysis, redo, and undo restart recovery
  - invalid WAL-tail truncation on startup

## Repository layout

```text
access/        Heap files, B+ trees, index interfaces
buffer/        Buffer pool and clock replacement
catalog/       Catalog and statistics metadata
common/        Shared types, schema, tuple, value utilities
concurrency/   Transactions, locks, deadlock handling, page retirement
execution/     Physical operators and expression evaluation
optimizer/     Cost model and plan enumeration
recovery/      WAL, checkpoints, restart recovery
sql/           SQL lexer, parser, binder, planner frontend
storage/       Disk manager, slotted pages, relation/table layer
tools/         Benchmarks, regression programs, crash demo
```

## Build

```bash
cmake -S . -B build
cmake --build build -j
```

## Quick demo

Run the core regression programs:

```bash
./build/db_sql_regression
./build/db_concurrency_regression
./build/db_recovery_regression /tmp/simpledb_recovery_regression
```

Open the SQL shell:

```bash
./build/db_sql_shell demo_db
```

Crash and recovery demo:

```bash
./build/db_crash_demo fresh /tmp/simpledb_demo 200
SIMPLEDB_CRASH_POINT=BEFORE_PAGE_FLUSH ./build/db_crash_demo fresh /tmp/simpledb_demo_crash 160
./build/db_crash_demo recover /tmp/simpledb_demo_crash
```

## Scope

This is an educational and research-oriented DB engine core, not a full production database.

Notable non-goals for the current codebase:

- MVCC / snapshot isolation
- parallel query execution
- online DDL
- full SQL coverage
- full production-level optimizer completeness
- media recovery / archive logging

## Suggested resume description

> Built a PostgreSQL-inspired database engine in C++ with slotted-page storage, B+ tree indexes, a clock buffer manager, physical query operators, a Selinger-style optimizer, strict 2PL concurrency control, and WAL-based crash recovery.
