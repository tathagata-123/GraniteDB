# GraniteDB

GraniteDB is a PostgreSQL-inspired database engine written in C++.

I built this project to understand database internals by implementing the main pieces together instead of studying them in isolation. The focus is on the core DBMS path: storage, indexing, buffer management, query execution, query optimization, transactions, concurrency control, and crash recovery.

This is not meant to be a production database or a clone of PostgreSQL. The goal is to build a coherent engine that is serious enough to study real design tradeoffs: page layout, access paths, join algorithms, locking, deadlocks, phantoms, WAL, and restart recovery.

---

## What GraniteDB implements

### Storage and indexing
- slotted-page heap storage
- free-space management
- table and relation layer for reading/writing tuples
- single-column and composite-key B+ tree indexes
- coordinated heap/index maintenance through the table layer

### Buffer management
- shared buffer pool
- clock replacement policy
- dirty-page tracking and flush support

### Query execution
- sequential scan
- index scan
- bitmap index scan and bitmap heap scan
- nested-loop join
- index nested-loop join
- memoized index nested-loop join
- hash join
- merge join
- sort
- projection
- filter
- aggregation
- set operations
- top-N / limit style operators

### Query optimization
- Selinger-style dynamic programming enumeration
- startup-cost vs total-cost tracking
- order-aware path retention
- histogram and MCV-based selectivity estimation
- pairwise extended statistics
- bitmap AND / OR planning

### Transactions and concurrency
- strict two-phase locking
- multiple-granularity locking
- lock upgrades
- deadlock detection
- key-range locking for phantom prevention on indexed scans

### Recovery
- write-ahead logging
- checkpoints
- restart recovery with analysis, redo, and undo
- invalid WAL-tail truncation on startup
- crash/restart test harnesses

---

## Why I built it this way

A lot of database projects stop after implementing a storage layer, or a parser, or one join algorithm. I wanted something more complete.

The main idea behind GraniteDB is that the interesting database behavior shows up in the interaction between subsystems:

- storage decisions affect access paths
- index structure affects locking and phantom prevention
- optimizer choices only make sense if the executor can support them
- recovery only matters if updates, logging, and page flushes interact correctly
- concurrency control is not meaningful unless it is tested against real access methods

So instead of treating each component as a separate assignment, I tried to make the project behave like one engine with connected responsibilities.

---

## High-level architecture

The repository is organized by subsystem.

```text
access/        Heap files, B+ trees, index interfaces
buffer/        Buffer pool and replacement policy
catalog/       Catalog and statistics metadata
common/        Shared types, schema, tuple, and value utilities
concurrency/   Transactions, locks, deadlock handling, page retirement
execution/     Physical operators and expression evaluation
optimizer/     Cost model and plan enumeration
recovery/      WAL, checkpoints, and restart recovery
sql/           SQL lexer, parser, binder, planner frontend
storage/       Disk manager, slotted pages, relation/table layer
tools/         Benchmarks, regression programs, crash demo
```

At a high level, the flow is:

1. SQL is parsed and bound
2. a logical/physical plan is built
3. the optimizer chooses an access path / plan shape
4. the executor runs operators over heap and index access methods
5. updates go through transaction, locking, and logging paths
6. dirty pages and WAL are coordinated through buffer / recovery components

---

## A few design choices

### 1. Strict 2PL instead of MVCC
I chose strict 2PL because I wanted to study locking, deadlocks, lock upgrades, phantom prevention, and the interaction between concurrency control and recovery. MVCC would have taken the design in a different direction.

### 2. B+ trees as the main index structure
I used B+ trees because they force you to deal with realistic issues:
- composite keys
- leaf/internal split logic
- merge / redistribution
- root changes
- index maintenance during updates
- key-range locking for phantom prevention

### 3. Cost-based optimization instead of fixed rule selection
I did not want the executor to always run one hard-coded path. Even in an educational engine, access path choice matters. So the optimizer keeps track of cost and interesting order properties and tries to pick plans that make sense for the query shape.

### 4. WAL and restart recovery as first-class parts of the engine
Recovery was not added as an afterthought. The point of a database engine is not just answering queries when everything goes well; it is also preserving correctness across crashes.

---

## What I paid the most attention to

The parts I spent the most time thinking about were:

### Locking correctness
Deadlock detection by itself is not hard. The hard part is getting the behavior around waiting, victim selection, rollback, and lock release correct enough that the engine still behaves like strict 2PL.

### Phantom prevention
Row locks are not enough for indexed range predicates. Once the engine supports index-based access, you need to think in terms of key ranges and gaps, not just tuple identifiers.

### Recovery under bad timing
Recovery only becomes real when the system can crash between logging and page flush, or in the middle of a multi-step update. I added crash points and restart tests specifically because the happy path is misleading.

### Keeping the project coherent
A database engine can become messy very quickly if storage, execution, optimizer, locking, and recovery all evolve independently. I tried to keep interfaces understandable and subsystem boundaries visible.

---

## Build

GraniteDB uses CMake.

```bash
cmake -S . -B build
cmake --build build -j
```

---

## Running the main regression programs

These are the programs I use most when checking the engine after changes.

### SQL regression
```bash
./build/db_sql_regression
```

### Concurrency regression
```bash
./build/db_concurrency_regression
```

### Recovery regression
```bash
./build/db_recovery_regression /tmp/granitedb_recovery_regression
```

### SQL shell
```bash
./build/db_sql_shell demo_db
```

### Benchmark tool
```bash
./build/db_bench /tmp/granitedb_bench 5000 5
```

### Crash / recovery demo
Fresh run:
```bash
./build/db_crash_demo fresh /tmp/granitedb_demo 200
```

Inject a crash at a configured crash point:
```bash
SIMPLEDB_CRASH_POINT=BEFORE_PAGE_FLUSH ./build/db_crash_demo fresh /tmp/granitedb_demo_crash 160
```

Recover:
```bash
./build/db_crash_demo recover /tmp/granitedb_demo_crash
```

---

## Example workflow I use while developing

A typical cycle for me is:

1. build the project
2. run SQL regression
3. run concurrency regression if I touched locking / access methods
4. run recovery regression if I touched logging / update paths
5. use the SQL shell for quick manual checks
6. use the crash demo when working on recovery edge cases

That sequence catches most accidental breakage quickly.

---

## What this project is and is not

### What it is
- a serious educational / research DBMS implementation
- a project meant to study internal DBMS mechanisms in code
- a system where storage, indexing, optimizer, locking, and recovery are meant to work together
- a codebase I can use to reason concretely about DBMS design choices

### What it is not
- a full PostgreSQL replacement
- a production-ready database
- a full SQL implementation
- an MVCC engine
- a distributed database
- a system optimized for production-scale throughput or operational deployment

I think it is better to be explicit about this. The point of the project is depth and correctness of the core engine path, not feature count for its own sake.

---

## Current limitations / non-goals

The following are intentionally out of scope for now:

- MVCC / snapshot isolation
- parallel query execution
- distributed execution or replication
- online DDL
- full production-grade optimizer completeness
- complete SQL coverage
- full media recovery / archive log support
- mature external tooling around deployment and observability

There are many directions this could be extended in, but I would rather keep the current scope clear than claim more than the implementation actually supports.

---

## Things I would improve next

If I continue developing GraniteDB, the areas I would most likely push further are:

1. stronger optimizer completeness and costing
2. broader SQL coverage
3. deeper recovery edge-case handling and validation
4. cleaner transaction integration across all execution paths
5. possibly an MVCC branch as a separate design direction

---

## Notes on style and implementation approach

I wrote this project to be studied. That does not mean every file is tiny or every subsystem is trivial, but I tried to keep the code understandable enough that I can revisit it later and still follow the main invariants.

In general I preferred:
- explicit subsystem boundaries
- data-structure-first implementations
- correctness-first behavior for transactions and recovery
- clear enough code to debug without heroics

---

## Repository walkthrough

### `storage/`
This is where the page-level storage mechanics live: disk interaction, slotted pages, relation/table storage, and tuple placement/update/deletion behavior.

### `access/`
This contains heap and B+ tree access methods. If you want to understand how tuples are reached efficiently, this is one of the first directories to study.

### `buffer/`
This is the buffer pool layer. It sits between logical access methods and durable storage and is important for page lifetime, dirtiness, and replacement behavior.

### `execution/`
This contains the physical operator layer: scans, joins, sort, aggregation, filters, projection, and expression evaluation.

### `optimizer/`
This is where plan enumeration and costing happen. It is the best place to look if you want to understand why one access path is chosen over another.

### `concurrency/`
This is the transaction and lock-management part of the engine: strict 2PL, upgrades, deadlock handling, and range-lock related logic.

### `recovery/`
This is the WAL and restart-recovery side: checkpoints, log replay, undo/redo, and startup recovery behavior.

### `sql/`
This is the SQL-facing side: lexer, parser, binder, and the frontend path into planning/execution.

### `tools/`
This contains the programs I use to exercise the system:
- regression binaries
- crash demo
- benchmark tool

---

## Suggested order for reading the code

If you are trying to understand the project as a system, this is the order I think makes the most sense:

1. `common/`  
   basic shared types and schema/value representation

2. `storage/` and `buffer/`  
   page layout, tuple storage, and buffer behavior

3. `access/`  
   heap and B+ tree access methods

4. `execution/`  
   how plans are actually run

5. `optimizer/`  
   how access paths and join plans are chosen

6. `concurrency/`  
   locking and transaction behavior

7. `recovery/`  
   WAL and restart logic

8. `sql/`  
   frontend layer that feeds the engine

---

## Closing note

GraniteDB is the project I built to force myself to understand database internals in implementation detail.

Reading textbooks helped, but the real learning came from getting the pieces to work together: page layout with indexes, indexes with locking, locking with recovery, and optimizer decisions with executor behavior. That is what this repository is really about.
