// procgen_tscheck.ts -- the TYPESCRIPT client's expectations, mirroring
// tool/procgen_pytest.py against the same fixture
// (tool/procgen_pynest_fixture.sql).  PLAN-TSGEN Q0: written before the
// emitter exists; its import of ./client.ts is the red state.
//
// Run by tool/procgen_tstest.py as:  node checks.ts DB ADDON
// Prints ALL OK on success; anything less is failure however it exits.

import { connect, flat, pwc, deep, bigv } from "./client.ts";

function fail(msg: string): never {
  console.error("FAIL: " + msg);
  process.exit(1);
}
function eq(got: unknown, want: unknown, what: string): void {
  const g = JSON.stringify(got, (_k, v) =>
    typeof v === "bigint" ? "bigint:" + v.toString() : v);
  const w = JSON.stringify(want, (_k, v) =>
    typeof v === "bigint" ? "bigint:" + v.toString() : v);
  if (g !== w) fail(`${what}\n  got:  ${g}\n  want: ${w}`);
  console.log(`${what} OK`);
}

const dbPath = process.argv[2];
const addonPath = process.argv[3];
if (!dbPath || !addonPath) fail("usage: node checks.ts DB ADDON");

const db = connect(dbPath, addonPath);

// flat, with a bound parameter
eq(flat(db, 1), [{ id: 2, title: "second" }], "flat");

// depth-1 nested: explicit RETURNING list, stitch from segments
eq(pwc(db), [
  { id: 1, title: "first", comments: [
    { post_id: 1, cid: 1, body: "a" },
    { post_id: 1, cid: 2, body: "b" }] },
  { id: 2, title: "second", comments: [
    { post_id: 2, cid: 3, body: "c" },
    { post_id: 2, cid: 4, body: "d" }] },
], "pwc depth-1");

// depth-2: RETURNING *, two-level stitch, empty leaf at cid=4
eq(deep(db), [
  { id: 1, title: "first", comments: [
    { post_id: 1, cid: 1, replies: [{ r_cid: 1, rid: 1, txt: "r1" }] },
    { post_id: 1, cid: 2, replies: [{ r_cid: 2, rid: 2, txt: "r2" }] }] },
  { id: 2, title: "second", comments: [
    { post_id: 2, cid: 3, replies: [{ r_cid: 3, rid: 3, txt: "r3" }] },
    { post_id: 2, cid: 4, replies: [] }] },
], "deep depth-2 with empty leaf");

// the JS-number boundary: 2^60 must arrive as BigInt, exactly
{
  const rows = bigv(db);
  if (rows.length !== 1) fail("bigv: expected one row");
  const v = rows[0].v;
  if (typeof v !== "bigint") fail(`bigv: expected bigint, got ${typeof v}`);
  if (v !== 1152921504606846976n) fail(`bigv: wrong value ${v}`);
  console.log("bigv BigInt boundary OK");
}

// and small integers stay plain numbers (the boundary cuts both ways)
{
  const rows = flat(db, 0);
  if (typeof rows[0].id !== "number") {
    fail(`small int should be number, got ${typeof rows[0].id}`);
  }
  console.log("small-int-as-number OK");
}

db.close();
console.log("ALL OK");
