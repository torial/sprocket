# REPL POC 1b: INCREMENTAL wal shipping -- the streaming model -- and
# its two suspected diseases: (a) checkpoint resets the wal under the
# shipper's saved offset; (b) wal2 alternates files under it.
file delete -force rp.db rp.db-wal rp.db-wal2 rp.db-shm
file delete -force rr.db rr.db-wal rr.db-wal2 rr.db-shm
sqlite3 db rp.db
db eval {PRAGMA journal_mode=wal2; CREATE TABLE t(a INTEGER PRIMARY KEY, b INTEGER)}
db eval {INSERT INTO t VALUES(0,0)}
# base snapshot: db file only (post-checkpoint quiesce)
db eval {PRAGMA wal_checkpoint(TRUNCATE)}
file copy -force rp.db rr.db

# incremental shipper state: bytes already shipped per wal file
array set off {rp.db-wal 0 rp.db-wal2 0}
proc ship_incr {} {
  global off
  foreach f {rp.db-wal rp.db-wal2} {
    set dst rr.db[string range $f 5 end]
    if {![file exists $f]} continue
    set sz [file size $f]
    if {$sz < $off($f)} {
      puts "  DISEASE(a): $f shrank ($off($f) -> $sz) -- wal reset under the shipper"
      set off($f) 0
      file delete -force $dst
    }
    if {$sz > $off($f)} {
      set in [open $f rb]; seek $in $off($f)
      set data [read $in [expr {$sz - $off($f)}]]
      close $in
      set out [open $dst {WRONLY CREAT APPEND BINARY}]
      puts -nonewline $out $data
      close $out
      set off($f) $sz
    }
  }
}
proc replica_state {} {
  set r ""
  catch {
    sqlite3 dbr rr.db
    set r [dbr eval {SELECT count(*), coalesce(sum(b),0) FROM t}]
    dbr close
  } msg
  if {$r eq ""} { return "ERR:[string range $msg 0 50]" }
  return $r
}

set nBad 0
for {set k 1} {$k <= 24} {incr k} {
  db eval {INSERT INTO t VALUES($k, $k*10)}
  if {$k == 12} { db eval {PRAGMA wal_checkpoint(TRUNCATE)} }
  ship_incr
  set want [db eval {SELECT count(*), coalesce(sum(b),0) FROM t}]
  set got [replica_state]
  if {$got ne $want} {
    if {$nBad < 3} { puts "  DIVERGED k=$k: want={$want} got={$got}" }
    incr nBad
  }
}
puts "incremental shipping diverged at $nBad/24 checkpoints"
db close
