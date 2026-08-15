# REPL POC 1: the naive ship-by-copy model, probed for its diseases.
# Primary in wal2; per commit, copy db+wal+wal2 to a replica and
# compare replica state to a from-scratch replay expectation (the
# temporal2 proof shape, reused as the replication instrument).
file delete -force rp.db rp.db-wal rp.db-wal2 rp.db-shm
file delete -force rr.db rr.db-wal rr.db-wal2 rr.db-shm
sqlite3 db rp.db
db eval {PRAGMA journal_mode=wal2; CREATE TABLE t(a INTEGER PRIMARY KEY, b INTEGER)}

proc ship {} {
  foreach ext {"" -wal -wal2} {
    catch {file copy -force rp.db$ext rr.db$ext}
  }
}
proc replica_state {} {
  set r ""
  catch {
    sqlite3 dbr rr.db
    set r [dbr eval {SELECT a, b FROM t ORDER BY a}]
    dbr close
  } msg
  if {$r eq ""} { return "ERR:$msg" }
  return $r
}

# workload with checkpoints; expectation from the live primary itself
set nBad 0
for {set k 1} {$k <= 30} {incr k} {
  db eval {INSERT INTO t VALUES($k, $k*10)}
  if {$k % 7 == 0} {
    # the checkpoint disease probe: truncate-checkpoint mid-stream
    db eval {PRAGMA wal_checkpoint(TRUNCATE)}
  }
  ship
  set want [db eval {SELECT a, b FROM t ORDER BY a}]
  set got [replica_state]
  if {$got ne $want} {
    puts "DIVERGED at k=$k: replica={[string range $got 0 60]}"
    incr nBad
  }
}
puts "ship-by-copy diverged at $nBad/30 checkpoints"

# disease 2: the shm question -- replica opened WITHOUT the shm copy
# (shm is per-host state; copying it is wrong; NOT copying it forces
# recovery every open).  Measure open cost signal: does replica open
# run recovery each time? (observable via needing write access)
file delete -force rr2.db rr2.db-wal rr2.db-wal2
file copy -force rp.db rr2.db
catch {file copy -force rp.db-wal rr2.db-wal}
catch {file copy -force rp.db-wal2 rr2.db-wal2}
sqlite3 dbr2 rr2.db -readonly 1
set rc [catch {dbr2 eval {SELECT count(*) FROM t}} msg]
puts "read-only replica open over shipped wal: rc=$rc $msg"
catch {dbr2 close}
db close
