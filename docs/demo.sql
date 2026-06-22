-- demo.sql — a guided tour of MiniDB. Pipe it in:  ./bin/minidb demo.db < docs/demo.sql
-- (Delete demo.db, demo.db.catalog, demo.db.wal first for a clean run.)

-- ---- DDL + storage engine ----
CREATE TABLE dept (id INT, dname VARCHAR, PRIMARY KEY (id));
CREATE TABLE emp  (id INT, name VARCHAR, dept INT, PRIMARY KEY (id));

-- ---- INSERT ----
INSERT INTO dept VALUES (10, 'Engineering'), (20, 'Sales'), (30, 'HR');
INSERT INTO emp  VALUES (1,'alice',10), (2,'bob',20), (3,'carol',10), (4,'dave',30);

-- ---- SELECT: projection, WHERE, ORDER BY ----
SELECT name, dept FROM emp WHERE dept = 10 ORDER BY name;

-- ---- JOIN (hash join, build on smaller side) ----
SELECT emp.name, dept.dname FROM emp JOIN dept ON emp.dept = dept.id ORDER BY emp.name;

-- ---- Aggregates ----
SELECT COUNT(*) FROM emp;
SELECT MAX(dept) FROM emp;

-- ---- Optimizer: EXPLAIN shows index vs seq scan and join build side ----
EXPLAIN SELECT * FROM emp WHERE id = 2;
EXPLAIN SELECT emp.name, dept.dname FROM emp JOIN dept ON emp.dept = dept.id;

-- ---- Transactions: rollback discards changes ----
BEGIN;
INSERT INTO emp VALUES (5, 'erin', 20);
SELECT COUNT(*) FROM emp;          -- 5 rows visible inside the txn
ROLLBACK;
SELECT COUNT(*) FROM emp;          -- back to 4

-- ---- DELETE ----
DELETE FROM emp WHERE id = 4;
SELECT id, name FROM emp ORDER BY id;

-- ---- Extension Track C: LSM-tree storage ----
CREATE TABLE kv (k INT, v VARCHAR, PRIMARY KEY (k)) USING LSM;
INSERT INTO kv VALUES (5,'five'), (2,'two'), (9,'nine'), (1,'one');
INSERT INTO kv VALUES (2, 'TWO-updated');   -- newest-wins
DELETE FROM kv WHERE k = 1;                  -- tombstone
SELECT * FROM kv;                            -- sorted, updated, key 1 gone
EXPLAIN SELECT * FROM kv;                    -- LSM merged scan

\q
