"""procgen_tstest.py -- proves the generated TYPESCRIPT client actually works.

Same contract as procgen_pytest.py, same fixture, same expectations --
different language and substrate: the client runs on Node over the N-API
addon (tool/napi), which statically links the fork.  PLAN-TSGEN Q0.

Run (from the repo root, after building the addon and procgen.exe):

    testfixture.exe <script loading procgen_pynest_fixture.sql into DB>
    python tool/procgen_tstest.py DB ADDON_DIR

where ADDON_DIR contains build/Release/sprocket_node.node and (once
`npm install typescript` has run there) node_modules/.bin/tsc.

Steps, each a gate:
  1. generate client.ts twice; the two bytes must be IDENTICAL
  2. tsc --noEmit over client + checks: the TYPE layer must check
  3. node runs the checks against the live db: values must match the
     fixture-derived expectations (flat+param, depth-1, depth-2 with
     empty leaf, and the BigInt boundary)

Every failure raises; the last line on success is ALL OK, and a run
that prints anything less is a failure however it exits.
"""
import os
import shutil
import subprocess
import sys


def run(cmd, **kw):
    print("+", " ".join(str(c) for c in cmd))
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode != 0:
        print(r.stdout)
        print(r.stderr, file=sys.stderr)
        raise SystemExit(f"FAILED ({r.returncode}): {' '.join(str(c) for c in cmd)}")
    return r


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: procgen_tstest.py DB ADDON_DIR", file=sys.stderr)
        return 2
    db_path, addon_dir = sys.argv[1:3]
    db_path = os.path.abspath(db_path)
    addon_dir = os.path.abspath(addon_dir)
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    procgen = os.path.join(root, "procgen.exe")
    addon = os.path.join(addon_dir, "build", "Release", "sprocket_node.node")
    tsc = os.path.join(addon_dir, "node_modules", ".bin",
                       "tsc.cmd" if os.name == "nt" else "tsc")
    checks_src = os.path.join(root, "tool", "procgen_tscheck.ts")

    for what, path in [("fixture db", db_path), ("addon", addon),
                       ("tsc", tsc), ("checks", checks_src),
                       ("procgen", procgen)]:
        if not os.path.exists(path):
            raise SystemExit(f"MISSING {what}: {path}")

    work = os.path.join(addon_dir, "tstest_work")
    os.makedirs(work, exist_ok=True)

    # 1 -- determinism: two generations, byte-identical
    g1 = run([procgen, "--lang", "ts", db_path]).stdout
    g2 = run([procgen, "--lang", "ts", db_path]).stdout
    if g1 != g2:
        raise SystemExit("REGENERATION NOT BYTE-IDENTICAL")
    if len(g1) < 500:
        raise SystemExit(f"generated client implausibly small ({len(g1)}b)")
    client = os.path.join(work, "client.ts")
    with open(client, "w", newline="") as f:
        f.write(g1)
    shutil.copy(checks_src, os.path.join(work, "checks.ts"))
    print(f"generated: {len(g1)} bytes, byte-identical on regen")

    # 2 -- the type layer must CHECK
    run([tsc, "--noEmit", "--strict", "--module", "nodenext",
         "--moduleResolution", "nodenext", "--target", "es2022",
         "--types", "node",
         os.path.join(work, "client.ts"), os.path.join(work, "checks.ts")],
        cwd=addon_dir)
    print("tsc --strict: clean")

    # 3 -- the values must MATCH, live
    r = run(["node", os.path.join(work, "checks.ts"), db_path, addon])
    print(r.stdout.rstrip())
    if "ALL OK" not in r.stdout:
        raise SystemExit("checks ran but did not print ALL OK")
    print("ALL OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
