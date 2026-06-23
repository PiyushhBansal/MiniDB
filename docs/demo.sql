-- demo.sql — a guided tour of MiniDB. Pipe it in:  ./bin/minidb demo.db < docs/demo.sql
-- (Delete demo.db, demo.db.catalog, demo.db.wal first for a clean run.)

-- ---- DDL + storage engine ----
CREATE TABLE dept (id INT, dname VARCHAR, PRIMARY KEY (id));
CREATE TABLE emp  (id INT, name VARCHAR, dept INT, PRIMARY KEY (id));

-- ---- INSERT ----
INSERT INTO dept VALUES (10, 'Engineering'), (20, 'Sales'), (30, 'HR');
INSERT INTO emp  VALUES (1,'alice',10), (2,'bob',20), (3,'carol',10), (4,'dave',30);

-- ---- SELECT: projection + WHERE ----
SELECT name, dept FROM emp WHERE dept = 10;

-- ---- JOIN (hash join, build on smaller side) ----
SELECT emp.name, dept.dname FROM emp JOIN dept ON emp.dept = dept.id;

-- ---- Optimizer: EXPLAIN shows index vs seq scan and join build side ----
EXPLAIN SELECT * FROM emp WHERE id = 2;
EXPLAIN SELECT emp.name, dept.dname FROM emp JOIN dept ON emp.dept = dept.id;

-- ---- Transactions: rollback discards changes ----
BEGIN;
INSERT INTO emp VALUES (5, 'erin', 20);
SELECT id, name FROM emp;          -- erin visible inside the txn
ROLLBACK;
SELECT id, name FROM emp;          -- erin gone after rollback

-- ---- DELETE ----
DELETE FROM emp WHERE id = 4;
SELECT id, name FROM emp;

-- ---- Extension Track B: MVCC ----
-- an MVCC table stores a version header (xmin/xmax) per row. readers use a
-- snapshot and never take locks; a deleted row is just stamped, not removed.
CREATE TABLE acct (id INT, name VARCHAR, bal INT, PRIMARY KEY (id)) USING MVCC;
INSERT INTO acct VALUES (1,'alice',100), (2,'bob',200), (3,'carol',300);
SELECT * FROM acct;
DELETE FROM acct WHERE id = 2;               -- stamps xmax, old snapshots still saw it
SELECT * FROM acct;                          -- bob no longer visible
EXPLAIN SELECT * FROM acct;                  -- MVCC scan (snapshot N)

\q
