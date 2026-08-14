# POC 2: KEPT AS THE LESSON -- this instrument is CIRCULAR (replay and
# asof read the same history table; its own control proved it blind to
# self-consistent corruption).  POC 2b is the honest version.  Original
# header follows.
# POC 2: the replay-equality instrument, built BEFORE the feature and
# proven able to go red.  Replay = fold history's closed+open intervals
# in valid_from order; AS OF t = interval containment.  On the
# hand-rolled pattern both are available; the equality is the theorem
# the real feature must pin.
file delete -force tpoc2.db tpoc2.db-wal tpoc2.db-wal2 tpoc2.db-shm
sqlite3 db tpoc2.db
db eval {
  PRAGMA journal_mode=wal2;
  CREATE TABLE acct(id INTEGER PRIMARY KEY, bal INTEGER);
  CREATE TABLE acct_hist(id INTEGER, bal INTEGER, seq INTEGER, op TEXT);
  -- seq, not wall-clock: the POC1 diseases are both clock-borne.  A
  -- per-STATEMENT sequence still leaks intra-txn states (disease 1) but
  -- gives replay a deterministic order to fold in.
  CREATE TRIGGER acct_ai AFTER INSERT ON acct BEGIN
    INSERT INTO acct_hist VALUES(NEW.id, NEW.bal,
      (SELECT coalesce(max(seq),0)+1 FROM acct_hist), 'I');
  END;
  CREATE TRIGGER acct_au AFTER UPDATE ON acct BEGIN
    INSERT INTO acct_hist VALUES(NEW.id, NEW.bal,
      (SELECT coalesce(max(seq),0)+1 FROM acct_hist), 'U');
  END;
  CREATE TRIGGER acct_ad AFTER DELETE ON acct BEGIN
    INSERT INTO acct_hist VALUES(OLD.id, NULL,
      (SELECT coalesce(max(seq),0)+1 FROM acct_hist), 'D');
  END;
}
# a working history: inserts, updates, a delete, re-insert
db eval {
  INSERT INTO acct VALUES(1,100);
  INSERT INTO acct VALUES(2,50);
  UPDATE acct SET bal=150 WHERE id=1;
  DELETE FROM acct WHERE id=2;
  UPDATE acct SET bal=175 WHERE id=1;
  INSERT INTO acct VALUES(2,60);
}

# AS OF seq S by REPLAY: fold ops 1..S
proc replay {S} {
  set state [dict create]
  db eval {SELECT id, bal, op FROM acct_hist WHERE seq <= $S ORDER BY seq} r {
    if {$r(op) eq "D"} { dict unset state $r(id) } \
    else               { dict set state $r(id) $r(bal) }
  }
  set out {}
  foreach id [lsort -integer [dict keys $state]] {
    lappend out "$id=[dict get $state $id]"
  }
  return $out
}
# AS OF seq S by LAST-WRITE-WINS lookup (the feature's query shape)
proc asof {S} {
  set out {}
  db eval {
    SELECT h.id AS hid, h.bal AS hbal FROM acct_hist h
     WHERE h.seq = (SELECT max(seq) FROM acct_hist
                     WHERE id = h.id AND seq <= $S)
       AND h.op != 'D'
     ORDER BY h.id
  } r { lappend out "$r(hid)=$r(hbal)" }
  return $out
}

set nSeq [db eval {SELECT max(seq) FROM acct_hist}]
set nBad 0
for {set S 1} {$S <= $nSeq} {incr S} {
  set a [replay $S]; set b [asof $S]
  if {$a ne $b} { puts "MISMATCH at seq $S: replay={$a} asof={$b}"; incr nBad }
}
puts "replay==asof at every seq 1..$nSeq: [expr {$nBad==0 ? "YES" : "NO ($nBad bad)"}]"
# and the PRESENT must equal the live table (the anchor to reality)
set live [db eval {SELECT id, bal FROM acct ORDER BY id}]
set lv {}
foreach {i b} $live { lappend lv "$i=$b" }
puts "asof(now)==live: [expr {[asof $nSeq] eq $lv ? "YES" : "NO"}]"

# CONTROL: corrupt one history row; the proof must go RED
db eval {UPDATE acct_hist SET bal=999 WHERE seq=3}
set red 0
for {set S 1} {$S <= $nSeq} {incr S} {
  if {[replay $S] ne [asof $S]} { set red 1 }
}
# replay and asof read the SAME corrupted row, so corruption that stays
# self-consistent cannot show here -- the anchor is the LIVE table:
puts "post-corruption asof(now)==live: [expr {[asof $nSeq] eq $lv ? "YES (instrument blind!)" : "NO (caught)"}]"
db close
