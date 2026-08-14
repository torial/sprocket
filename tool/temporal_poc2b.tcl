# POC 2b: the HONEST replay proof -- from-scratch WORKLOAD replay per
# checkpoint vs AS OF on the temporal side.  Independent by
# construction: the replay never reads the history table.
file delete -force tp_t.db tp_t.db-wal tp_t.db-wal2 tp_t.db-shm
set WORKLOAD {
  {INSERT INTO acct VALUES(1,100)}
  {INSERT INTO acct VALUES(2,50)}
  {UPDATE acct SET bal=150 WHERE id=1}
  {DELETE FROM acct WHERE id=2}
  {UPDATE acct SET bal=175 WHERE id=1}
  {INSERT INTO acct VALUES(2,60)}
}
sqlite3 dbt tp_t.db
dbt eval {
  CREATE TABLE acct(id INTEGER PRIMARY KEY, bal INTEGER);
  CREATE TABLE acct_hist(id INTEGER, bal INTEGER, seq INTEGER, op TEXT);
  CREATE TRIGGER acct_ai AFTER INSERT ON acct BEGIN
    INSERT INTO acct_hist VALUES(NEW.id, NEW.bal,
      (SELECT coalesce(max(seq),0)+1 FROM acct_hist), 'I'); END;
  CREATE TRIGGER acct_au AFTER UPDATE ON acct BEGIN
    INSERT INTO acct_hist VALUES(NEW.id, NEW.bal,
      (SELECT coalesce(max(seq),0)+1 FROM acct_hist), 'U'); END;
  CREATE TRIGGER acct_ad AFTER DELETE ON acct BEGIN
    INSERT INTO acct_hist VALUES(OLD.id, NULL,
      (SELECT coalesce(max(seq),0)+1 FROM acct_hist), 'D'); END;
}
foreach stmt $WORKLOAD { dbt eval $stmt }

proc asof {S} {
  set out {}
  dbt eval {
    SELECT h.id AS hid, h.bal AS hbal FROM acct_hist h
     WHERE h.seq = (SELECT max(seq) FROM acct_hist
                     WHERE id = h.id AND seq <= $S)
       AND h.op != 'D'
     ORDER BY h.id
  } r { lappend out "$r(hid)=$r(hbal)" }
  return $out
}

set nBad 0
for {set k 1} {$k <= [llength $WORKLOAD]} {incr k} {
  file delete -force tp_r.db
  sqlite3 dbr tp_r.db
  dbr eval {CREATE TABLE acct(id INTEGER PRIMARY KEY, bal INTEGER)}
  for {set j 0} {$j < $k} {incr j} { dbr eval [lindex $WORKLOAD $j] }
  set want {}
  dbr eval {SELECT id, bal FROM acct ORDER BY id} r {
    lappend want "$r(id)=$r(bal)"
  }
  dbr close
  set got [asof $k]
  if {$got ne $want} { puts "MISMATCH at k=$k: asof={$got} replay={$want}"; incr nBad }
}
puts "workload-replay == AS OF at every checkpoint: [expr {$nBad==0 ? "YES" : "NO"}]"

# CONTROL: the same corruption POC 2 could not see -- now it must be caught
dbt eval {UPDATE acct_hist SET bal=999 WHERE seq=3}
set caught 0
for {set k 1} {$k <= [llength $WORKLOAD]} {incr k} {
  file delete -force tp_r.db
  sqlite3 dbr tp_r.db
  dbr eval {CREATE TABLE acct(id INTEGER PRIMARY KEY, bal INTEGER)}
  for {set j 0} {$j < $k} {incr j} { dbr eval [lindex $WORKLOAD $j] }
  set want {}
  dbr eval {SELECT id, bal FROM acct ORDER BY id} r {
    lappend want "$r(id)=$r(bal)"
  }
  dbr close
  if {[asof $k] ne $want} { set caught 1 }
}
puts "CONTROL: corrupted interior history caught: [expr {$caught ? "YES (seen red)" : "NO (still blind)"}]"
dbt close
file delete -force tp_r.db
