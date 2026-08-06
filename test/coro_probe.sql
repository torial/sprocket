CREATE TABLE src(a INTEGER);
INSERT INTO src VALUES(1),(2),(3),(4),(5);
CREATE TABLE dst(a INTEGER);
CREATE TABLE log(n INTEGER);

-- CONTROL A: coroutine (INSERT...SELECT) inside a TRIGGER SubProgram
CREATE TRIGGER tr AFTER INSERT ON log BEGIN
  INSERT INTO dst SELECT a FROM src ORDER BY a;
END;
INSERT INTO log VALUES(1);
SELECT 'trigger-coroutine', count(*) FROM dst;

-- CONTROL B: negative control -- this MUST print 5, not 3.
--   If the probe printed a wrong number here I would know it is not counting.
DELETE FROM dst;
INSERT INTO dst SELECT a FROM src WHERE a<=3;
SELECT 'negative-control-expect-3', count(*) FROM dst;

-- SUBJECT: coroutine inside a PROCEDURE SubProgram
DELETE FROM dst;
CREATE PROCEDURE fill()
BEGIN
  INSERT INTO dst SELECT a FROM src ORDER BY a;
END;
CALL fill();
SELECT 'proc-coroutine', count(*) FROM dst;
