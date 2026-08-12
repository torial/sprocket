-- Fixture for tool/procgen_test.c -- the database its 17 checks run against.
--
-- Rebuild with:   testfixture (or the fork shell) reading this file into
--                 pgfix.db, then:  procgen.exe pgfix.db > pgclient.h
--
-- This file exists because the fixture used to live only as an untracked
-- pgfix.db in the repo root, which a later procgen session silently
-- overwrote with a different schema -- discovered 2026-08-11 when
-- procgen_test.c stopped compiling against the header generated from it.
-- The contract below is reverse-engineered from procgen_test.c's checks
-- and must keep matching them:
--   greet(who TEXT, n INTEGER) -> one row (msg TEXT, cnt INTEGER),
--     cnt echoing n (the harness binds 42 and expects 42);
--   two() -> set 1 (a INTEGER) holding 7, set 2 (b TEXT) holding 'seven',
--     sets of DIFFERENT widths so per-set accessors are exercised.

CREATE TABLE t1(a INTEGER);
INSERT INTO t1 VALUES(7);
CREATE TABLE t2(b TEXT);
INSERT INTO t2 VALUES('seven');

CREATE PROCEDURE greet(who TEXT, n INTEGER)
  RETURNS TABLE(msg TEXT, cnt INTEGER)
BEGIN
  SELECT 'hello, ' || who, n;
END;

CREATE PROCEDURE two()
  RETURNS TABLE(a INTEGER)
  RETURNS TABLE(b TEXT)
BEGIN
  SELECT a FROM t1;
  SELECT b FROM t2;
END;
