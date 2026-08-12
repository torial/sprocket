-- Fixture for tool/procgen_pytest.py -- the nested-shapes fixture the
-- generated PYTHON client is verified against (the C harness has its own,
-- tool/procgen_fixture.sql, with a different contract).
--
-- Three procedure shapes, one of each the emitter distinguishes:
--   flat(n)  -- no nesting: plain CALL, bound parameter;
--   pwc()    -- depth 1: explicit RETURNING value-column list, one stitch;
--   deep()   -- depth 2: RETURNING *, two-level stitch, and comment cid=4
--               has NO replies so the empty-leaf path is exercised.

CREATE TABLE posts(id INTEGER PRIMARY KEY, title TEXT);
CREATE TABLE comments(cid INTEGER PRIMARY KEY, post_id INTEGER, body TEXT);
CREATE TABLE replies(rid INTEGER PRIMARY KEY, r_cid INTEGER, txt TEXT);
INSERT INTO posts VALUES(1,'first'),(2,'second');
INSERT INTO comments VALUES(1,1,'a'),(2,1,'b'),(3,2,'c'),(4,2,'d');
INSERT INTO replies VALUES(1,1,'r1'),(2,2,'r2'),(3,3,'r3');

CREATE PROCEDURE flat(n INTEGER) RETURNS TABLE(id INTEGER, title TEXT)
BEGIN
  SELECT id, title FROM posts WHERE id > n;
END;

CREATE PROCEDURE pwc()
  RETURNS TABLE(id INTEGER, title TEXT,
                comments TABLE(post_id INTEGER, cid INTEGER, body TEXT)
                  KEY(post_id = id))
BEGIN
  SELECT id, title FROM posts;
  SELECT post_id, cid, body FROM comments;
END;

CREATE PROCEDURE deep()
  RETURNS TABLE(id INTEGER, title TEXT,
                comments TABLE(post_id INTEGER, cid INTEGER,
                               replies TABLE(r_cid INTEGER, rid INTEGER, txt TEXT)
                                 KEY(r_cid = cid))
                  KEY(post_id = id))
BEGIN
  SELECT id, title FROM posts;
  SELECT post_id, cid FROM comments;
  SELECT r_cid, rid, txt FROM replies;
END;
