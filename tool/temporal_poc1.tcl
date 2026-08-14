# POC 1: the hand-rolled temporal pattern (shadow history + triggers),
# and the commit-order anomaly that no trigger can see.
file delete -force tpoc.db tpoc.db-wal tpoc.db-wal2 tpoc.db-shm
sqlite3 db tpoc.db
db eval {
  PRAGMA journal_mode=wal2;
  CREATE TABLE acct(id INTEGER PRIMARY KEY, bal INTEGER);
  CREATE TABLE acct_hist(
    id INTEGER, bal INTEGER,
    valid_from TEXT, valid_to TEXT   -- the universal spelling
  );
  CREATE TRIGGER acct_ai AFTER INSERT ON acct BEGIN
    INSERT INTO acct_hist VALUES(NEW.id, NEW.bal,
        strftime('%Y-%m-%d %H:%M:%f','now'), NULL);
  END;
  CREATE TRIGGER acct_au AFTER UPDATE ON acct BEGIN
    UPDATE acct_hist SET valid_to = strftime('%Y-%m-%d %H:%M:%f','now')
      WHERE id = NEW.id AND valid_to IS NULL;
    INSERT INTO acct_hist VALUES(NEW.id, NEW.bal,
        strftime('%Y-%m-%d %H:%M:%f','now'), NULL);
  END;
  INSERT INTO acct VALUES(1, 100);
}

# --- Anomaly 1: same-transaction updates create ZERO-WIDTH intervals
# (or worse, two open rows if the clock does not tick between them).
db eval {
  BEGIN;
    UPDATE acct SET bal=200 WHERE id=1;
    UPDATE acct SET bal=300 WHERE id=1;
  COMMIT;
}
puts "hist after same-txn double update:"
db eval {SELECT bal, valid_from, valid_to FROM acct_hist ORDER BY rowid} r {
  puts "  bal=$r(bal) from=$r(valid_from) to=$r(valid_to)"
}
set nOpen [db eval {SELECT count(*) FROM acct_hist WHERE valid_to IS NULL}]
puts "open rows (must be exactly 1): $nOpen"

# --- Anomaly 2: commit order vs timestamp order.  Connection B stamps
# LATER than A but commits FIRST; A holds its write open across B's
# commit... (wal: one writer at a time -- so demonstrate with A stamping
# early via a long transaction: A begins, updates (stamp t1), sleeps;
# nothing else can write meanwhile in wal -- the anomaly needs TWO
# tables or... in a single-writer engine the interleave is:
# A: BEGIN, UPDATE x (stamp tA), ... COMMIT at tA+delta
# during [tA, tA+delta) readers see the OLD state, but AS OF tA+eps
# resolves to the NEW state after commit.  Measure that window.
db eval {BEGIN}
db eval {UPDATE acct SET bal=400 WHERE id=1}
set stamp [db eval {SELECT valid_from FROM acct_hist WHERE valid_to IS NULL}]
# a second connection reads NOW -- mid-A -- and sees bal=300
sqlite3 db2 tpoc.db
set liveMidTxn [db2 eval {SELECT bal FROM acct}]
after 120
db eval {COMMIT}
set commitWall [db eval {SELECT strftime('%Y-%m-%d %H:%M:%f','now')}]
puts "A stamped valid_from=$stamp; committed ~$commitWall"
puts "reader DURING A's txn saw bal=$liveMidTxn (the truth of that moment)"
# AS OF a time inside (stamp, commit): the history says 400 was current...
# ISO strings at equal precision compare lexicographically; datetime()
# TRUNCATES milliseconds and broke the first version of this probe.
set T [db2 eval {SELECT strftime('%Y-%m-%d %H:%M:%f', julianday($stamp) + 0.05/86400.0)}]
set asof [db2 eval {SELECT bal FROM acct_hist
                     WHERE valid_from <= $T
                       AND (valid_to IS NULL OR valid_to > $T)}]
puts "AS OF stamp+50ms the history answers bal=$asof"
puts "=> history asserts a state (bal=400) at a moment when every live reader saw bal=300"
db2 close
db close
