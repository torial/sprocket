CREATE TABLE t1(a INTEGER); INSERT INTO t1 VALUES(1),(3),(5);
CREATE TABLE t2(a INTEGER); INSERT INTO t2 VALUES(2),(4),(6);
CREATE PROCEDURE merged()
BEGIN
  SELECT a FROM t1 UNION ALL SELECT a FROM t2 ORDER BY a;
END;
.print === output: merged=123456, concatenated=135246 ===
CALL merged();
.print === InitCoroutine count inside the Program frame ===
EXPLAIN CALL merged();
