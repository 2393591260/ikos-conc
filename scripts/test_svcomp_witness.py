#!/usr/bin/env python3
"""Directed checks for the SV-COMP 2.2 wrapper's instance and failure contract."""

import argparse
import copy
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "svcomp_witness", ROOT / "svcomp_witness.py"
)
wrapper = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(wrapper)

SOURCE = """#include <pthread.h>
int shared;
void *worker(void *arg) {
  shared = 1;
  return 0;
}
void spawn(pthread_t *handle) {
  pthread_create(handle, 0, worker, 0);
}
void *parent(void *arg) {
  pthread_t handle;
  pthread_create(&handle, 0, worker, 0);
  shared = 2;
  return 0;
}
int main(void) {
  pthread_t first, second, third;
  pthread_create(&first, 0, parent, 0);
  spawn(&second);
  spawn(&third);
  shared = 3;
  return 0;
}
"""


class WitnessTests(unittest.TestCase):
    ikos = ROOT / "install/bin/ikos"

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="test-ikos-witness-")
        self.addCleanup(self.temp.cleanup)
        self.folder = Path(self.temp.name)
        self.source = self.folder / "renamed.c"
        self.source.write_text(SOURCE)

    def instruction(self, fragment, kind="store", entry=None):
        lines = self.source.read_text().splitlines()
        line = next(i for i, text in enumerate(lines, 1) if fragment in text)
        text = lines[line - 1]
        column = text.index("pthread_create" if kind == "call" else "=") + 1
        result = dict(file=str(self.source), line=line, column=column, kind=kind)
        if kind == "call":
            result["operands"] = [
                dict(kind="function", id="pthread_create"),
                {},
                {},
                dict(kind="function", id=entry),
                {},
            ]
        return result

    def pair(self):
        instructions = {
            "helper": self.instruction("pthread_create(handle", "call", "worker"),
            "root": self.instruction("pthread_create(&first", "call", "parent"),
            "nested": self.instruction("pthread_create(&handle", "call", "worker"),
            "worker": self.instruction("shared = 1"),
            "parent": self.instruction("shared = 2"),
            "main": self.instruction("shared = 3"),
        }
        return dict(
            access_a=dict(thread_id="worker"),
            access_b=dict(thread_id="worker"),
            # Deliberately wrong legacy names/order: dynamic replay owns identity.
            thread_creations=[dict(thread_function="worker", creator="main")],
            counterexample=dict(
                schema=1,
                program=dict(
                    instructions=instructions,
                    functions={
                        name: {} for name in ("main", "worker", "parent", "spawn")
                    },
                    globals={"shared": {}},
                ),
                traces=[
                    dict(
                        id="startup",
                        entry="main",
                        steps=[
                            dict(statement="helper"),
                            dict(statement="helper"),
                            dict(statement="helper"),
                        ],
                    ),
                    dict(
                        id="a",
                        entry="worker",
                        creator_trace="startup",
                        creator_step=1,
                        steps=[dict(statement="worker", clock="0")],
                    ),
                    dict(
                        id="b",
                        entry="worker",
                        creator_trace="startup",
                        creator_step=2,
                        steps=[dict(statement="worker", clock="1")],
                    ),
                ],
                targets=[dict(trace="b", step=0), dict(trace="a", step=0)],
            ),
        )

    def content(self, pair):
        return wrapper.build_witness(
            str(self.source), pair, "ILP32", str(self.ikos)
        )[0]["content"]

    def test_repeated_helper_and_later_same_entry_instances(self):
        content = self.content(self.pair())
        self.assertEqual(
            [s["segment"][0]["waypoint"]["thread_id"] for s in content[:-1]], [0, 0, 0]
        )
        targets = [w["waypoint"] for w in content[-1]["segment"]]
        self.assertEqual([w["thread_id"] for w in targets], [3, 2])
        self.assertTrue(
            all(w["location"]["file_name"] == self.source.name for w in targets)
        )
        self.assertEqual(targets[0]["location"]["column"], 3)
        create = content[0]["segment"][0]["waypoint"]["location"]
        self.assertEqual(
            create["column"], self.source.read_text().splitlines()[7].rindex(")") + 1
        )

    def test_main_child(self):
        pair = self.pair()
        c = pair["counterexample"]
        c["traces"] = [
            dict(
                id="a",
                entry="main",
                steps=[
                    dict(statement="helper", clock="0"),
                    dict(statement="main", clock="2"),
                ],
            ),
            dict(
                id="b",
                entry="worker",
                creator_trace="a",
                creator_step=0,
                steps=[dict(statement="worker", clock="1")],
            ),
        ]
        c["targets"] = [dict(trace="a", step=1), dict(trace="b", step=0)]
        self.assertEqual(
            [w["waypoint"]["thread_id"] for w in self.content(pair)[-1]["segment"]],
            [0, 1],
        )

    def test_dynamic_parent_instance(self):
        pair = self.pair()
        pair["counterexample"]["traces"] = [
            dict(id="startup", entry="main", steps=[dict(statement="root")]),
            dict(
                id="a",
                entry="parent",
                creator_trace="startup",
                creator_step=0,
                steps=[
                    dict(statement="nested", clock="0"),
                    dict(statement="parent", clock="1"),
                ],
            ),
            dict(
                id="b",
                entry="worker",
                creator_trace="a",
                creator_step=0,
                steps=[dict(statement="worker", clock="2")],
            ),
        ]
        pair["counterexample"]["targets"] = [
            dict(trace="a", step=1),
            dict(trace="b", step=0),
        ]
        content = self.content(pair)
        self.assertEqual(
            [s["segment"][0]["waypoint"]["thread_id"] for s in content[:-1]], [0, 1]
        )
        self.assertEqual(
            [w["waypoint"]["thread_id"] for w in content[-1]["segment"]], [1, 2]
        )

    def test_invalid_instance_evidence_rejected(self):
        mutations = {
            "missing counterexample": lambda p: p.pop("counterexample"),
            "missing creator": lambda p: p["counterexample"]["traces"][1].pop(
                "creator_trace"
            ),
            "unknown creator": lambda p: p["counterexample"]["traces"][1].update(
                creator_trace="absent"
            ),
            "invalid creator step": lambda p: p["counterexample"]["traces"][1].update(
                creator_step=100
            ),
            "collapsed children": lambda p: p["counterexample"]["traces"][2].update(
                creator_step=1
            ),
            "wrong entry": lambda p: p["counterexample"]["traces"][1].update(
                entry="parent"
            ),
            "duplicate trace id": lambda p: p["counterexample"]["traces"][2].update(
                id="a"
            ),
            "duplicate clocks": lambda p: p["counterexample"]["traces"][2]["steps"][
                0
            ].update(clock="0"),
            "missing target": lambda p: p["counterexample"]["targets"].pop(),
            "wrong target step": lambda p: p["counterexample"]["targets"][0].update(
                step=100
            ),
            "same target thread": lambda p: p["counterexample"]["targets"][0].update(
                trace="a"
            ),
            "noncreation creator": lambda p: p["counterexample"]["program"][
                "instructions"
            ]["helper"].update(kind="store"),
            "indirect entry": lambda p: p["counterexample"]["program"]["instructions"][
                "helper"
            ]["operands"][3].update(kind="variable"),
        }
        for name, mutate in mutations.items():
            with self.subTest(name=name):
                pair = self.pair()
                mutate(pair)
                with self.assertRaises((ValueError, KeyError, TypeError)):
                    self.content(pair)

    def test_clock_order_and_selected_spawn_after_target_rejected(self):
        pair = self.pair()
        pair["counterexample"]["traces"] = [
            dict(id="startup", entry="main", steps=[dict(statement="root")]),
            dict(
                id="a",
                entry="parent",
                creator_trace="startup",
                creator_step=0,
                steps=[
                    dict(statement="parent", clock="0"),
                    dict(statement="nested", clock="1"),
                ],
            ),
            dict(
                id="b",
                entry="worker",
                creator_trace="a",
                creator_step=1,
                steps=[dict(statement="worker", clock="2")],
            ),
        ]
        pair["counterexample"]["targets"] = [
            dict(trace="a", step=0),
            dict(trace="b", step=0),
        ]
        with self.assertRaisesRegex(ValueError, "creation follows"):
            self.content(pair)
        pair["counterexample"]["traces"][2]["steps"][0]["clock"] = "-1"
        with self.assertRaisesRegex(ValueError, "before its creation"):
            self.content(pair)

    def source_map(self):
        sm = wrapper.SourceMap(str(self.source))
        sm.load_ast(str(self.ikos), "ILP32")
        return sm

    def test_source_mapping_and_exact_call_positions(self):
        logical = self.folder / "logical.c"
        self.source = self.folder / "input.i"
        self.source.write_text(
            "typedef unsigned pthread_t; int shared, other; "
            "int pthread_create(pthread_t*,void*,void*(*)(void*),void*); void*worker(void*);\n"
            '# 100 "' + str(logical) + '"\n'
            "void body(void) {\n"
            "  pthread_t a, b;\n"
            "  pthread_create(&a, 0, worker, 0); pthread_create(&b,\n"
            "    0, worker, 0);\n"
            "  shared =\n"
            "    other;\n"
            "}\n"
        )
        sm = self.source_map()
        first = dict(file=str(logical), line=102, column=3)
        text = self.source.read_text().splitlines()
        second = dict(first, column=text[4].index("pthread_create", 3) + 1)
        self.assertEqual(
            sm.creation(first),
            dict(file_name="input.i", line=5, column=text[4].index(")") + 1),
        )
        self.assertEqual(
            sm.creation(second),
            dict(file_name="input.i", line=6, column=text[5].index(")") + 1),
        )
        self.assertEqual(
            sm.target(dict(file=str(logical), line=105, column=5)),
            dict(file_name="input.i", line=7, column=3),
        )
        with self.assertRaisesRegex(ValueError, "absent or ambiguous"):
            sm.position(
                dict(
                    file=str(self.folder / "elsewhere" / "logical.c"),
                    line=102,
                    column=3,
                )
            )
        self.source.write_text(
            '#line 5 "' + str(logical) + '"\nshared = 1;\n'
            '#line 5 "' + str(logical) + '"\nshared = 2;\n'
        )
        with self.assertRaisesRegex(ValueError, "absent or ambiguous"):
            wrapper.SourceMap(str(self.source)).position(
                dict(file=str(logical), line=5, column=1)
            )

    def test_source_target_conditions_and_same_line_statements(self):
        self.source.write_text(
            "int shared, other;\nvoid body(void) { if (shared) other = 1; shared = 2; }\n"
        )
        text = self.source.read_text().splitlines()[1]
        sm = self.source_map()
        condition = text.index("shared") + 1
        self.assertEqual(
            sm.target(dict(file=str(self.source), line=2, column=condition))["column"],
            condition,
        )
        second_store = text.index("= 2") + 1
        self.assertEqual(
            sm.target(dict(file=str(self.source), line=2, column=second_store))[
                "column"
            ],
            text.index("shared", condition) + 1,
        )

    def test_ast_handles_compound_literals_typedefs_macros_and_splicing(self):
        for body, expected in [
            ("other = (int){shared};", "other"),
            ("T*local = (T*)shared;", "(T*)"),
            ("T(local) = shared;", "shared"),
            ("COND(shared) other = 1;", "shared"),
            ("i\\\nf(shared) other = 1;", "shared"),
        ]:
            with self.subTest(body=body):
                text = (
                    "int shared, other; typedef int T;\n#define COND if\nvoid body(void) { "
                    + body
                    + " }\n"
                )
                self.source.write_text(text)
                lines = text.splitlines()
                line = next(
                    i for i, content in enumerate(lines[2:], 3) if "shared" in content
                )
                column = lines[line - 1].index("shared") + 1
                target = self.source_map().target(
                    dict(file=str(self.source), line=line, column=column)
                )
                expected_offset = text.index(expected, text.index("void body"))
                expected_line = text[:expected_offset].count("\n") + 1
                expected_column = expected_offset - text.rfind("\n", 0, expected_offset)
                self.assertEqual(
                    target,
                    dict(
                        file_name=self.source.name,
                        line=expected_line,
                        column=expected_column,
                    ),
                )
        self.source.write_text(
            "int shared;\n#define ACCESS shared = 1; shared = 2\n"
            "void body(void) { ACCESS; }\n"
        )
        with self.assertRaisesRegex(ValueError, "no unique"):
            self.source_map().target(dict(file=str(self.source), line=3, column=18))
        self.source.write_text('#if 0\n#line 20 "other.c"\n#endif\nint shared;\n')
        with self.assertRaisesRegex(ValueError, "linemarkers"):
            wrapper.SourceMap(str(self.source))

    def test_foreign_files_token_pasting_and_aggregate_initializers_rejected(self):
        header = self.folder / "body.h"
        header.write_text("static void body(void) { shared = 1; }\n")
        self.source.write_text('int shared;\n#include "body.h"\n')
        sm = self.source_map()
        with self.assertRaisesRegex(ValueError, "absent or ambiguous"):
            sm.target(dict(file=str(header), line=1, column=33))
        self.source.write_text(
            "int shared;\n#define CAT(a,b) a##b\n"
            "void body(void) { CAT(sha,red) = 1; }\n"
        )
        with self.assertRaisesRegex(ValueError, "no unique"):
            self.source_map().target(dict(file=str(self.source), line=3, column=18))
        self.source.write_text(
            "int shared; void body(void) { int local[2] = {[1]=shared}; }\n"
        )
        text = self.source.read_text()
        with self.assertRaisesRegex(ValueError, "no unique"):
            self.source_map().target(
                dict(file=str(self.source), line=1, column=text.index("shared", 11) + 1)
            )
        self.source.write_text('#define N 10\n#line N "logical.c"\nint shared;\n')
        with self.assertRaisesRegex(ValueError, "linemarker syntax"):
            wrapper.SourceMap(str(self.source))

    def test_commented_markers_and_utf8_byte_columns(self):
        self.source.write_text(
            'int shared, other;\nvoid body(void) {\n/*\n#line 4 "'
            + str(self.source)
            + '"\n*/\n  shared = 1;\n  other = 2;\n}\n'
        )
        target = self.source_map().target(
            dict(file=str(self.source), line=6, column=10)
        )
        self.assertEqual(target, dict(file_name=self.source.name, line=6, column=3))
        text = "int shared, other; void body(void) { /*éééééééééé*/ shared = 1; other = 2; }"
        self.source.write_text(text)
        column = len(text[: text.index("=")].encode("utf-8")) + 1
        target = self.source_map().target(
            dict(file=str(self.source), line=1, column=column)
        )
        self.assertEqual(
            target["column"], text.index("shared", text.index("void body")) + 1
        )

    def test_c_vertical_whitespace_is_not_a_physical_line_break(self):
        text = "int shared, other;\nvoid body(void) {\vshared = 1;\f other = 2; }\n"
        self.source.write_text(text)
        physical = text.encode("utf-8").splitlines()[1]
        target = self.source_map().target(
            dict(file=str(self.source), line=2, column=physical.index(b"=") + 1)
        )
        self.assertEqual(
            target,
            dict(
                file_name=self.source.name, line=2, column=physical.index(b"shared") + 1
            ),
        )

    def test_carriage_return_comment_lines_preserve_marker_mapping(self):
        logical = self.folder / "logical.c"
        text = (
            "int shared;\rvoid body(void) {\r/*a\rb*/\r"
            '#line 100 "' + str(logical) + '"\rshared = 1;\r}\r'
        )
        self.source.write_bytes(text.encode("utf-8"))
        target = self.source_map().target(dict(file=str(logical), line=100, column=8))
        self.assertEqual(target, dict(file_name=self.source.name, line=6, column=1))

    def test_first_race_skips_may_and_unknown(self):
        pair = self.pair()
        reports = [
            dict(status=1, info=pair),
            dict(status=2, info=dict(pair, verdict="unknown")),
            dict(status=2, info=pair),
        ]
        self.assertIs(wrapper.first_race(dict(reports=reports)), pair)
        self.assertIsNone(wrapper.first_race(dict(reports=reports[:2])))
        with self.assertRaises(ValueError):
            wrapper.first_race({})

    def run_wrapper(self, mode, pair=None, out=None, extra_options=(), inventory=1):
        fake = self.folder / "fake-ikos"
        fake.write_text(r"""#!/usr/bin/env python3
import json, os, pathlib, sys
report = next(x.split("=",1)[1] for x in sys.argv if x.startswith("--report-file="))
db = sys.argv[sys.argv.index("-o")+1]
pathlib.Path(os.environ["FAKE_PATHS"]).write_text(json.dumps([report,db,db+"-wal",db+"-shm"]))
pathlib.Path(os.environ["FAKE_ARGV"]).write_text(json.dumps(sys.argv[1:]))
with pathlib.Path(os.environ["FAKE_ARGV"] + '.calls').open('a') as calls:
 calls.write(json.dumps(sys.argv[1:]) + '\n')
for p in (db,db+"-wal",db+"-shm"): pathlib.Path(p).write_text("temp")
for arg in sys.argv:
 if arg.startswith('--allocator-inventory='):
  value = json.loads(os.environ['FAKE_INVENTORY'])
  if value is not None:
   pathlib.Path(arg.split('=', 1)[1]).write_text(json.dumps({'schema': 1, 'eligible_calls': value}))
mode = os.environ["FAKE_MODE"]
if mode.startswith('refine-'):
 mode = mode[len('refine-'):] if '--inline-allocators' in sys.argv else 'unknown'
if mode == "changed":
 pathlib.Path(sys.argv[-1]).write_text(pathlib.Path(sys.argv[-1]).read_text()+"\n/*changed*/\n")
if mode == "crash": print("The program is definitely UNSAFE", file=sys.stderr)
if mode != "missing":
 pathlib.Path(report).write_text("{" if mode == "json" else pathlib.Path(os.environ["FAKE_REPORT"]).read_text())
if mode == "silent":
    pass
elif mode == "conflict":
    print("The program is UNKNOWN\nThe program is SAFE")
elif mode == "unknown":
    print("The program is UNKNOWN")
else:
    print("The program is SAFE" if mode in ("safe","json") else "The program is definitely UNSAFE")
sys.exit(3 if mode == "crash" else 0)
""")
        fake.chmod(0o755)
        config = self.folder / "ikos-config"
        frontend = subprocess.run(
            [str(self.ikos.parent / "ikos-config"), "--clang", "--includedir"],
            capture_output=True, text=True, check=True,
        ).stdout.splitlines()
        self.assertEqual(len(frontend), 2)
        config.write_text(
            "#!/usr/bin/env python3\nprint("
            + repr(frontend[0])
            + ")\nprint("
            + repr(frontend[1])
            + ")\n"
        )
        if mode == "config":
            config.write_text("#!/usr/bin/env python3\nraise SystemExit(2)\n")
        config.chmod(0o755)
        report = self.folder / "fake-report.json"
        report.write_text(
            json.dumps(
                dict(reports=[] if pair is None else [dict(status=2, info=pair)])
            )
        )
        out = out or self.folder / "witness.yml"
        if out.parent.exists():
            out.write_text("obsolete FALSE witness")
        paths = self.folder / "temporary-paths.json"
        env = dict(
            os.environ,
            FAKE_MODE=mode,
            FAKE_INVENTORY=json.dumps(inventory),
            FAKE_REPORT=str(report),
            FAKE_PATHS=str(paths),
            FAKE_ARGV=str(self.folder / "analyzer-arguments.json"),
        )
        proc = subprocess.run(
            [
                sys.executable,
                str(ROOT / "svcomp_witness.py"),
                str(self.source),
                "--ikos",
                str(fake),
                "--out",
                str(out),
                *extra_options,
            ],
            env=env,
            text=True,
            capture_output=True,
        )
        for p in json.loads(paths.read_text()):
            self.assertFalse(Path(p).exists(), "analysis temporary file survived")
        self.assertFalse(
            list(self.folder.glob(".ikos-witness-*")), "partial witness survived"
        )
        return proc, out

    def test_default_budgets_inherit_analyzer_settings(self):
        proc, _ = self.run_wrapper("safe")
        self.assertEqual(proc.returncode, 0, proc.stderr)
        argv = json.loads((self.folder / "analyzer-arguments.json").read_text())
        self.assertFalse(any(arg.startswith("--bmc-") for arg in argv))
        self.assertNotIn("--inline-allocators", argv)

    def test_allocator_inlining_reaches_analyzer(self):
        proc, _ = self.run_wrapper("safe", extra_options=("--inline-allocators",))
        self.assertEqual(proc.returncode, 0, proc.stderr)
        argv = json.loads((self.folder / "analyzer-arguments.json").read_text())
        self.assertEqual(argv.count("--inline-allocators"), 1)
        self.assertNotIn("--inline-all", argv)
        self.assertEqual(argv[-1], str(self.source))

    def test_allocator_refinement_is_unknown_only(self):
        for mode, expected in (("safe", 1), ("unknown", 2), ("refine-safe", 2)):
            with self.subTest(mode=mode):
                calls = self.folder / 'analyzer-arguments.json.calls'
                calls.unlink(missing_ok=True)
                proc, out = self.run_wrapper(mode, extra_options=("--try-inline-allocators",))
                self.assertEqual(proc.returncode, 0, proc.stderr)
                rows = [json.loads(x) for x in calls.read_text().splitlines()]
                self.assertEqual(len(rows), expected)
                self.assertNotIn('--inline-allocators', rows[0])
                if expected == 2:
                    self.assertIn('--inline-allocators', rows[1])
                if mode == 'refine-safe':
                    self.assertNotIn('The program is UNKNOWN', proc.stdout)
                    self.assertIn('The program is SAFE', proc.stdout)
                self.assertFalse(out.exists())

    def test_no_constructor_skips_refinement(self):
        proc, out = self.run_wrapper("unknown", inventory=0,
                                    extra_options=("--try-inline-allocators",))
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(proc.stdout.strip(), 'The program is UNKNOWN')
        self.assertEqual(len((self.folder / 'analyzer-arguments.json.calls').read_text().splitlines()), 1)
        self.assertFalse(out.exists())

    def test_invalid_constructor_inventory_is_error(self):
        for value in (None, -1, True, "1", {}):
            with self.subTest(value=value):
                calls = self.folder / 'analyzer-arguments.json.calls'
                calls.unlink(missing_ok=True)
                proc, out = self.run_wrapper("unknown", inventory=value,
                                            extra_options=("--try-inline-allocators",))
                self.assertNotEqual(proc.returncode, 0)
                self.assertEqual(len(calls.read_text().splitlines()), 1)
                self.assertFalse(out.exists())

    def test_refinement_failure_and_malformed_output_remain_errors(self):
        for mode, count in (("refine-crash", 2), ("refine-missing", 2),
                            ("silent", 1), ("conflict", 1), ("crash", 1)):
            with self.subTest(mode=mode):
                calls = self.folder / 'analyzer-arguments.json.calls'
                calls.unlink(missing_ok=True)
                proc, out = self.run_wrapper(mode, extra_options=("--try-inline-allocators",))
                self.assertNotEqual(proc.returncode, 0)
                self.assertEqual(len(calls.read_text().splitlines()), count)
                self.assertFalse(out.exists())

    def test_inlining_modes_are_mutually_exclusive(self):
        proc = subprocess.run([sys.executable, str(ROOT / 'svcomp_witness.py'),
            '--inline-allocators', '--try-inline-allocators', '--version'],
            capture_output=True, text=True)
        self.assertEqual(proc.returncode, 2)
        self.assertIn('not allowed with argument', proc.stderr)

    def test_explicit_budgets_reach_analyzer(self):
        for deadline in (0, 60000):
            with self.subTest(deadline=deadline):
                proc, _ = self.run_wrapper("safe", extra_options=(
                    "--bmc-deadline-ms", str(deadline),
                    "--bmc-solver-timeout-ms=3000",
                ))
                self.assertEqual(proc.returncode, 0, proc.stderr)
                argv = json.loads((self.folder / "analyzer-arguments.json").read_text())
                self.assertIn("--bmc-deadline-ms={}".format(deadline), argv)
                self.assertIn("--bmc-solver-timeout-ms=3000", argv)
                self.assertEqual(argv[-1], str(self.source))

    def test_invalid_budgets_fail_before_analysis(self):
        for flag, value in (("--bmc-deadline-ms", "-1"),
                            ("--bmc-solver-timeout-ms", "0"),
                            ("--bmc-solver-timeout-ms", "-1")):
            with self.subTest(flag=flag, value=value):
                proc = subprocess.run([
                    sys.executable, str(ROOT / "svcomp_witness.py"),
                    str(self.source), "--ikos", "absent", flag + "=" + value,
                ], capture_output=True, text=True)
                self.assertEqual(proc.returncode, 2)
                self.assertEqual(proc.stdout, "")
                self.assertIn(flag + " must be", proc.stderr)

    def test_success_writes_witness_before_false(self):
        proc, out = self.run_wrapper("race", self.pair())
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("definitely UNSAFE", proc.stdout)
        witness = json.loads(out.read_text())
        self.assertEqual(
            witness[0]["metadata"]["task"]["input_files"], [self.source.name]
        )
        self.assertEqual(
            [w["waypoint"]["thread_id"] for w in witness[0]["content"][-1]["segment"]],
            [3, 2],
        )

    def test_safe_clears_stale_witness(self):
        proc, out = self.run_wrapper("safe")
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("The program is SAFE", proc.stdout)
        self.assertFalse(out.exists())

    def test_unknown_is_a_successful_inconclusive_result(self):
        proc, out = self.run_wrapper("unknown")
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertEqual(proc.stdout, "The program is UNKNOWN\n")
        self.assertEqual(proc.stderr, "")
        self.assertFalse(out.exists())

    def test_failures_emit_only_errors_and_clean_files(self):
        missing = self.pair()
        missing.pop("counterexample")
        for mode, pair, output in [
            ("race", missing, None),
            ("race", None, None),
            ("json", None, None),
            ("missing", None, None),
            ("crash", self.pair(), None),
            ("race", self.pair(), self.folder / "missing-directory" / "witness.yml"),
            ("config", self.pair(), None),
            ("changed", self.pair(), None),
        ]:
            with self.subTest(mode=mode, output=output, missing=pair is None):
                proc, out = self.run_wrapper(mode, pair, output)
                self.assertNotEqual(proc.returncode, 0)
                self.assertEqual(proc.stdout, "")
                self.assertIn("error: cannot produce a competition result:", proc.stderr)
                self.assertNotIn("definitely UNSAFE", proc.stderr)
                self.assertFalse(out.exists())

    def test_ast_failure_does_not_publish_false(self):
        pair = self.pair()
        self.source.write_text(SOURCE + "\nint invalid = ;\n")
        proc, out = self.run_wrapper("race", pair)
        self.assertNotEqual(proc.returncode, 0)
        self.assertEqual(proc.stdout, "")
        self.assertIn("Clang AST extraction failed", proc.stderr)
        self.assertFalse(out.exists())

    def test_output_cannot_delete_source(self):
        before = self.source.read_bytes()
        proc = subprocess.run(
            [
                sys.executable,
                str(ROOT / "svcomp_witness.py"),
                str(self.source),
                "--ikos",
                "absent",
                "--out",
                str(self.source),
            ],
            capture_output=True,
            text=True,
        )
        self.assertNotEqual(proc.returncode, 0)
        self.assertEqual(proc.stdout, "")
        self.assertEqual(self.source.read_bytes(), before)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--ikos", type=Path, default=WitnessTests.ikos)
    args, remaining = parser.parse_known_args()
    WitnessTests.ikos = args.ikos.resolve()
    unittest.main(argv=[sys.argv[0], *remaining])
