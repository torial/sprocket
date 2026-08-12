"""procgen_pytest.py -- proves the generated PYTHON client actually works.

Same contract as tool/procgen_test.c: the generated client must do what
hand-written access would, against a live database, through the fork's own
engine -- not merely import.  Pairs with tool/procgen_pynest_fixture.sql,
whose data these expectations are derived from.

Run (from the repo root, after building sqlite3.dll and procgen.exe):

    testfixture.exe  <script that loads procgen_pynest_fixture.sql into DB>
    procgen.exe DB --lang python > CLIENT.py
    python tool/procgen_pytest.py DB sqlite3.dll CLIENT.py

Every check raises on failure; the last line printed on success is ALL OK,
and a run that prints anything less is a failure however it exits.
"""
import importlib.util
import sys

def main() -> int:
    if len(sys.argv) != 4:
        print("usage: procgen_pytest.py DB DLL CLIENT.py", file=sys.stderr)
        return 2
    db_path, dll_path, client_path = sys.argv[1:4]

    spec = importlib.util.spec_from_file_location("pyclient", client_path)
    pc = importlib.util.module_from_spec(spec)
    # dataclasses resolves the client's postponed annotations through
    # sys.modules[cls.__module__]; loading without registering there makes
    # every @dataclass in the client blow up.
    sys.modules["pyclient"] = pc
    spec.loader.exec_module(pc)

    db = pc.connect(db_path, dll_path)

    # flat, with a bound parameter
    f = pc.flat(db, 1)
    assert f == [pc.FlatRow(id=2, title="second")], f
    print("flat OK:", f)

    # depth-1 nested: explicit RETURNING list, stitch from segments
    p = pc.pwc(db)
    expect = [
        pc.PwcRow(id=1, title="first", comments=[
            pc.PwcComments(post_id=1, cid=1, body="a"),
            pc.PwcComments(post_id=1, cid=2, body="b")]),
        pc.PwcRow(id=2, title="second", comments=[
            pc.PwcComments(post_id=2, cid=3, body="c"),
            pc.PwcComments(post_id=2, cid=4, body="d")]),
    ]
    assert p == expect, p
    print("pwc OK: 2 parents, 2+2 children")

    # depth-2: RETURNING *, two-level stitch, empty leaf included
    d = pc.deep(db)
    expect = [
        pc.DeepRow(id=1, title="first", comments=[
            pc.DeepComments(post_id=1, cid=1, replies=[
                pc.DeepReplies(r_cid=1, rid=1, txt="r1")]),
            pc.DeepComments(post_id=1, cid=2, replies=[
                pc.DeepReplies(r_cid=2, rid=2, txt="r2")])]),
        pc.DeepRow(id=2, title="second", comments=[
            pc.DeepComments(post_id=2, cid=3, replies=[
                pc.DeepReplies(r_cid=3, rid=3, txt="r3")]),
            pc.DeepComments(post_id=2, cid=4, replies=[])]),
    ]
    assert d == expect, d
    print("deep OK: depth-2 stitch, RETURNING *, empty leaf included")

    # NEGATIVE CONTROL: the comparator must be able to fail
    assert d != expect[:1], "comparator cannot distinguish"
    print("negative control OK")

    # refusal path is loud, not silent
    try:
        db._segments("CALL nosuch()")
        print("FAIL: no error for a missing procedure", file=sys.stderr)
        return 1
    except pc.ProcError as e:
        print("refusal OK:", e)

    db.close()
    print("ALL OK")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
