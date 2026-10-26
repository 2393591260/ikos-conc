#!/usr/bin/env python3
"""Self-check the local GraphML adapter; optionally write validator controls."""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import tempfile
import xml.etree.ElementTree as ET

from validate_bmc_witnesses import classify, export_graphml, NS, thread_ids, projected_steps

SOURCE = """#include <pthread.h>
int x;
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
void *a(void *p) {
  BEFORE
  x = 1;
  AFTER
  return 0;
}
void *b(void *p) {
  BEFORE
  x = 2;
  AFTER
  return 0;
}
int main(void) {
  pthread_t ta, tb;
  pthread_create(&ta, 0, a, 0);
  pthread_create(&tb, 0, b, 0);
  pthread_join(ta, 0);
  pthread_join(tb, 0);
  return 0;
}
"""


def control(source, model, task):
    lines = source.read_text().splitlines()
    def location(fragment):
        return dict(line=next(i for i, line in enumerate(lines, 1) if fragment in line), file=str(source))
    pair = dict(access_a=dict(location("x = 1"), thread_id="a", kind=1),
                access_b=dict(location("x = 2"), thread_id="b", kind=1),
                thread_creations=[dict(location("pthread_create(&ta"), thread_function="a", creator="main"),
                                  dict(location("pthread_create(&tb"), thread_function="b", creator="main")])
    return dict(task=task, input=str(source), data_model=model,
                input_sha256=hashlib.sha256(source.read_bytes()).hexdigest(), confirmed_pairs=[pair])


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--controls", type=Path, help="write controls and manifest here for --mode targets")
    p.add_argument("--check-results", type=Path, action="append", help="check recorded validator controls")
    args = p.parse_args()
    with tempfile.TemporaryDirectory() as temp:
        source = Path(temp) / "renamed.c"
        source.write_text(SOURCE.replace("BEFORE", "").replace("AFTER", ""))
        row = control(source, "ILP32", "renamed")
        graph = Path(temp) / "witness.graphml"
        export_graphml(row, row["confirmed_pairs"][0], "targets", graph)
        assert ET.parse(graph).getroot().tag == "{" + NS + "}graphml"
        pair = copy.deepcopy(row["confirmed_pairs"][0])
        a, b = pair["access_a"], pair["access_b"]
        pair["confirmed_path_a"] = [dict(a, ar="load @x", clock="0"),
                                    dict(a, ar="store @x, 1", clock="2")]
        pair["confirmed_path_b"] = [dict(b, ar="store @x, 2", clock="1")]
        pair["confirmed_startup"] = [dict(file=str(source), line=2, ar="store @x, 0", clock="startup_0")]
        pair["confirmed_startup"] += [dict(c, ar="call @pthread_create(...)", clock="startup_" + str(i + 1))
                                      for i, c in enumerate(pair["thread_creations"])]
        export_graphml(row, pair, "prefix", graph)
        line_tags = ET.parse(graph).findall(".//{" + NS + "}data[@key='startline']")
        assert [int(tag.text) for tag in line_tags].count(a["line"]) == 1, "split AR expression required two source executions"
        steps = projected_steps(pair, "prefix")
        assert steps[0]["ar"] == "store @x, 0", "creation moved before initialization"
        assert [s["create"] for s in steps if "create" in s] == [1, 2]
        missing = copy.deepcopy(pair)
        missing["confirmed_startup"] = []
        try:
            projected_steps(missing, "prefix")
        except ValueError:
            pass
        else:
            raise AssertionError("missing creation prefix fabricated")
        for field in ("counterexample", "replay"):
            timed = copy.deepcopy(row["confirmed_pairs"][0])
            timed[field] = dict(traces=[dict(id="main", entry="main"),
                                       dict(id="provider", entry="provider")])
            for mode in ("targets", "prefix"):
                try:
                    projected_steps(timed, mode)
                except ValueError as exc:
                    assert str(exc) == (
                        "timed main/provider source projection is unsupported by the legacy adapter"), exc
                else:
                    raise AssertionError("timed provider was silently projected as a legacy startup")
        for field, other in (("counterexample", "replay"), ("replay", "counterexample")):
            timed = copy.deepcopy(row["confirmed_pairs"][0])
            timed[field] = dict(traces=[dict(id="main", entry="main")])
            timed[other] = dict(traces=[dict(id="startup", entry="main")])
            for mode in ("targets", "prefix"):
                try:
                    projected_steps(timed, mode)
                except ValueError as exc:
                    assert str(exc) == (
                        "timed main/provider source projection is unsupported by the legacy adapter"), exc
                else:
                    raise AssertionError("one replay alias hid the other timed execution")
            for malformed in (None, [], {}, dict(traces=None), dict(traces=[]),
                              dict(traces=[None]), dict(traces=[{}])):
                broken = copy.deepcopy(row["confirmed_pairs"][0])
                broken[field] = malformed
                for mode in ("targets", "prefix"):
                    try:
                        projected_steps(broken, mode)
                    except ValueError as exc:
                        assert str(exc).startswith("malformed " + field), exc
                    else:
                        raise AssertionError("malformed replay provenance was ignored")
        pair = copy.deepcopy(row["confirmed_pairs"][0])
        pair["access_b"]["thread_id"] = "a"
        try:
            thread_ids(pair)
        except ValueError:
            pass
        else:
            raise AssertionError("one creation accepted as two instances")
        pair["thread_creations"][1]["thread_function"] = "a"
        assert thread_ids(pair) == [1, 2]
        source.write_text(source.read_text() + "\n")
        try:
            export_graphml(row, row["confirmed_pairs"][0], "targets", graph)
        except ValueError:
            pass
        else:
            raise AssertionError("changed source accepted")
    assert classify("cpachecker", "Verification result: FALSE.", 0, False) == "ACCEPT"
    assert classify("cpachecker", "Verification result: FALSE.", 1, False) == "ERROR"
    assert classify("dartagnan", "Result: FAIL", 0, True) == "TIMEOUT"
    assert classify("dartagnan", "Result: UNKNOWN", 0, False) == "UNKNOWN"
    print("PASS: hash, distinct instance identity, source expression projection, XML, and result classification")
    for file in args.check_results or []:
        for row in json.loads(file.read_text()):
            name = row["task"]
            if name.startswith(("locked-", "exclusive-")):
                assert all(row[t]["status"] == "REJECT" for t in ("cpachecker", "dartagnan")), row
            elif name.startswith("race-"):
                assert all(row[t]["status"] == "ACCEPT" for t in ("cpachecker", "dartagnan")), row
            elif name.startswith(("wrong-location-", "nonconflicting-targets-")):
                assert row["cpachecker"]["status"] == "REJECT", row
            if name.startswith(("wrong-", "nonconflicting-")):
                print("Calibration:", name, {t: row[t]["status"] for t in ("cpachecker", "dartagnan")})
        print("PASS: recorded positive/negative controls", file)
    if args.controls:
        folder = args.controls.resolve()
        folder.mkdir(parents=True, exist_ok=False)
        variants = {"race": SOURCE.replace("BEFORE", "").replace("AFTER", ""),
                    "locked": SOURCE.replace("BEFORE", "pthread_mutex_lock(&lock);").replace("AFTER", "pthread_mutex_unlock(&lock);")}
        historical = Path(__file__).resolve().parents[1] / "docs/review-repros/bmc-2026-10-08/exclusive-flag.c"
        variants["exclusive"] = historical.read_text()
        rows = []
        for name, text in variants.items():
            source = folder / (name + ".c")
            source.write_text(text)
            for model in ("ILP32", "LP64"):
                rows.append(control(source, model, name + "-" + model))
        for model in ("ILP32", "LP64"):
            row = control(folder / "race.c", model, "wrong-location-" + model)
            for side in ("a", "b"):
                row["confirmed_pairs"][0]["access_" + side]["line"] = 999999
            rows.append(row)
            row = control(folder / "race.c", model, "wrong-thread-" + model)
            # Export an intentionally corrupted creation binding: known race,
            # but b's line is attributed to a and vice versa.
            row["confirmed_pairs"][0]["access_a"]["thread_id"] = "b"
            row["confirmed_pairs"][0]["access_b"]["thread_id"] = "a"
            rows.append(row)
        source = folder / "other-race.c"
        text = variants["race"].replace("int x;", "int x, y, z;").replace("x = 1;", "x = 1;\n  y = 1;").replace("x = 2;", "x = 2;\n  z = 2;")
        source.write_text(text)
        for model in ("ILP32", "LP64"):
            row = control(source, model, "nonconflicting-targets-" + model)
            for side, fragment in (("a", "y = 1"), ("b", "z = 2")):
                row["confirmed_pairs"][0]["access_" + side]["line"] = next(i for i, line in enumerate(text.splitlines(), 1) if fragment in line)
            rows.append(row)
        (folder / "pairs.json").write_text(json.dumps(rows, indent=2) + "\n")
        print("Controls written:", folder / "pairs.json")


if __name__ == "__main__":
    main()
