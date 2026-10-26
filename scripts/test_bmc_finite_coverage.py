#!/usr/bin/env python3
"""Exhaustively compare a fixed finite SC/mutex language with BMC and strict AR.

The oracle interprets its own events and enumerates every enabled interleaving.
It does not import IKOS candidates, unrolling, encoders, SMT or AR semantics.
Its finite input language is deliberately narrower than C: two workers; three
zero-initialized global int objects; read/write events; optional outer ordinary
mutex sections; 1..3 fixed loop iterations; immutable Boolean branch input.
There are no atomics, malloc, pointer arithmetic, payload arithmetic, blocking
cycles, data-dependent branches, or pthread failures in this promised subset.
SAFE/UNKNOWN on an oracle-safe case is allowed; bounded absence is never proof.
Every oracle-racy case must be FALSE with an independently accepted execution
whose actual dynamic targets belong to an oracle-racy event pair.
"""

import argparse
from collections import Counter
from dataclasses import asdict, dataclass
import hashlib
import itertools
import json
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


@dataclass(frozen=True)
class Case:
    name: str
    counts: tuple = (1, 1)
    operations: tuple = ('W', 'W')
    shared: bool = True
    locks: tuple = (-1, -1)
    guards: tuple = (-1, -1)
    input: int = 0
    direct: bool = False


@dataclass(frozen=True)
class Event:
    thread: int
    ordinal: int
    operation: str
    object: int

    @property
    def identity(self):
        return (self.thread, self.ordinal, self.operation, self.object)


def cases():
    """Closed Cartesian families; these bounds define the coverage claim."""
    lock_modes = ((-1, -1), (0, 0), (0, -1), (0, 1))
    for operations, shared, locks in itertools.product(
            itertools.product('RW', repeat=2), (False, True), lock_modes):
        yield Case('single-' + ''.join(operations) + '-' + str(int(shared)) +
                   '-' + str(lock_modes.index(locks)), operations=operations,
                   shared=shared, locks=locks)
    for left, right, locked, direct in itertools.product(
            (1, 2, 3), (1, 2, 3), (False, True), (False, True)):
        yield Case(f'later-{left}-{right}-{int(locked)}' + ('-direct' if direct else ''),
                   counts=(left, right), operations=('W', 'R'),
                   locks=(0, 0) if locked else (-1, -1), direct=direct)
    for left, right, value, locked in itertools.product((0, 1), repeat=4):
        yield Case(f'guard-{left}-{right}-{value}-{locked}',
                   guards=(left, right), input=value,
                   locks=(0, 0) if locked else (-1, -1))


def events(case):
    threads = []
    for thread in (0, 1):
        active = case.guards[thread] in (-1, case.input)
        sequence = []
        if active:
            lock = case.locks[thread]
            if lock >= 0:
                sequence.append(Event(thread, 0, 'L', lock))
            for index in range(case.counts[thread]):
                shared = case.shared and index == case.counts[thread] - 1
                sequence.append(Event(thread, index + 1, case.operations[thread],
                                      0 if shared else thread + 1))
            if lock >= 0:
                sequence.append(Event(thread, 0, 'U', lock))
        threads.append(tuple(sequence))
    return tuple(threads)


def ordered(left, right):
    return all(a <= b for a, b in zip(left, right))


def enumerate_schedules(program):
    """Visit every complete enabled schedule, retaining all racy dynamic pairs.

    The two vector-clock components start after the common main spawn prefix.
    Mutex acquire joins its last release, then every event ticks its own clock.
    Access records are never merged or supplied by the production analyzer.
    C loop control, immutable-input reads and call/return scaffolding commute
    with the other thread and cannot race: schedules count retained events.
    """
    schedules = 0
    racing_schedules = 0
    pairs = set()
    first_race_schedule = None

    def visit(pc, clocks, owners, releases, accesses, races, schedule):
        nonlocal schedules, racing_schedules, first_race_schedule
        if all(pc[t] == len(program[t]) for t in (0, 1)):
            schedules += 1
            if races:
                racing_schedules += 1
                pairs.update(races)
                if first_race_schedule is None:
                    first_race_schedule = schedule
            return
        enabled = 0
        for thread in (0, 1):
            if pc[thread] == len(program[thread]):
                continue
            event = program[thread][pc[thread]]
            if event.operation == 'L' and event.object in owners:
                continue
            enabled += 1
            next_owners, next_releases = dict(owners), dict(releases)
            clock = list(clocks[thread])
            if event.operation == 'L':
                clock = [max(a, b) for a, b in zip(clock, releases.get(event.object, (0, 0)))]
                next_owners[event.object] = thread
            clock[thread] += 1
            next_clocks = list(clocks)
            next_clocks[thread] = tuple(clock)
            next_accesses, next_races = accesses, set(races)
            if event.operation == 'U':
                assert owners.get(event.object) == thread, 'unmatched oracle unlock'
                del next_owners[event.object]
                next_releases[event.object] = tuple(clock)
            elif event.operation in ('R', 'W'):
                for previous, previous_clock in accesses:
                    if (previous.thread != thread and previous.object == event.object and
                            'W' in (previous.operation, event.operation) and
                            not ordered(previous_clock, clock)):
                        pair = tuple(sorted((previous.identity, event.identity)))
                        next_races.add(pair)
                next_accesses = accesses + ((event, tuple(clock)),)
            next_pc = list(pc)
            next_pc[thread] += 1
            visit(tuple(next_pc), tuple(next_clocks), next_owners, next_releases,
                  next_accesses, next_races, schedule + (thread,))
        assert enabled, 'unexpected deadlock outside finite language'

    visit((0, 0), ((0, 0), (0, 0)), {}, {}, (), set(), ())
    return dict(schedules=schedules, racing_schedules=racing_schedules,
                pairs=sorted(pairs), first_race_schedule=first_race_schedule)


def source(case):
    lines = [
        '#include <pthread.h>',
        'volatile int payload0, payload1, payload2;',
        f'volatile int immutable_input = {case.input};',
        'pthread_mutex_t mutex0 = PTHREAD_MUTEX_INITIALIZER;',
        'pthread_mutex_t mutex1 = PTHREAD_MUTEX_INITIALIZER;',
        '__attribute__((noinline)) void emit_write(volatile int *p) {',
        '  *p = 1;',
        '}',
        '__attribute__((noinline)) void emit_read(volatile int *p) {',
        '  (void)*p;',
        '}',
    ]
    # Mapping is produced before C is analyzed, independently of AR targets.
    access_lines = {}
    for thread in (0, 1):
        lines += [f'void *worker{thread}(void *ignored) {{', '  (void)ignored;']
        if case.guards[thread] >= 0:
            lines += [f'  if (immutable_input != {case.guards[thread]}) return 0;']
        if case.locks[thread] >= 0:
            lines += [f'  pthread_mutex_lock(&mutex{case.locks[thread]});']
        count = case.counts[thread]
        call = 'emit_write' if case.operations[thread] == 'W' else 'emit_read'
        # A noinline helper keeps the same static access at every iteration.
        # Distinct private globals force the unique cross-thread pair to be late.
        pointer = f'i == {count - 1} ? &payload0 : &payload{thread + 1}' if case.shared else f'&payload{thread + 1}'
        lines += [f'  for (int i = 0; i < {count}; ++i) {{']
        if case.direct:
            lines += [f'    volatile int *p = {pointer};']
            lines += ['    *p = 1;' if case.operations[thread] == 'W' else '    (void)*p;']
            line = len(lines)
        else:
            lines += [f'    {call}({pointer});']
            line = 7 if case.operations[thread] == 'W' else 10
        access_lines[f'worker{thread}'] = dict(operation=case.operations[thread], line=line)
        lines += ['  }']
        if case.locks[thread] >= 0:
            lines += [f'  pthread_mutex_unlock(&mutex{case.locks[thread]});']
        lines += ['  return 0;', '}']
    lines += ['int main(void) {', '  pthread_t left, right;',
              '  pthread_create(&left, 0, worker0, 0);',
              '  pthread_create(&right, 0, worker1, 0);',
              '  pthread_join(left, 0);', '  pthread_join(right, 0);',
              '  return 0;', '}']
    return '\n'.join(lines) + '\n', access_lines


def compare(oracle, observation):
    """One comparator for real runs and deliberate omissions/corruptions."""
    verdict = observation['verdict']
    if verdict not in ('SAFE', 'UNKNOWN', 'FALSE'):
        raise AssertionError('incomplete/error execution: ' + verdict)
    expected = {tuple(tuple(event) for event in pair) for pair in oracle['pairs']}
    actual = {tuple(tuple(event) for event in pair) for pair in observation['pairs']}
    if expected and verdict != 'FALSE':
        raise AssertionError('oracle-racy input was not confirmed: ' + verdict)
    if verdict == 'FALSE' and not actual:
        raise AssertionError('FALSE lacks mapped actual dynamic targets')
    if not actual <= expected:
        raise AssertionError('selected dynamic targets are absent from oracle-racy pairs')
    if not expected and verdict == 'FALSE':
        raise AssertionError('oracle-safe input reported FALSE')


def self_test():
    collection = list(cases())
    assert len(collection) == 84 and len({c.name for c in collection}) == 84
    totals = Counter()
    for case in collection:
        oracle = enumerate_schedules(events(case))
        assert oracle['schedules'] > 0
        totals['inputs'] += 1
        totals['schedules'] += oracle['schedules']
        totals['racy_inputs'] += bool(oracle['pairs'])
        totals['racy_schedules'] += oracle['racing_schedules']
        code, mapping = source(case)
        for access in mapping.values():
            fragment = '*p = 1;' if access['operation'] == 'W' else '(void)*p;'
            assert fragment in code.splitlines()[access['line'] - 1]
        # The unlocked unique late pair has exactly these dynamic ordinals.
        if case.name.startswith('later-') and case.locks == (-1, -1):
            assert oracle['pairs'] == [((0, case.counts[0], 'W', 0),
                                        (1, case.counts[1], 'R', 0))]
        elif case.locks == (0, 0):
            assert not oracle['pairs']
        # A second, non-scheduling characterization for this exact language:
        # without a common outer mutex every cross-thread conflicting access
        # is unordered; a common outer mutex orders the whole access sequence.
        independent_pairs = set()
        if not (case.locks[0] >= 0 and case.locks[0] == case.locks[1]):
            left_events, right_events = events(case)
            for left, right in itertools.product(left_events, right_events):
                if (left.operation in 'RW' and right.operation in 'RW' and
                        left.object == right.object and
                        'W' in (left.operation, right.operation)):
                    independent_pairs.add((left.identity, right.identity))
        assert set(oracle['pairs']) == independent_pairs, case
        compare(oracle, dict(verdict='FALSE' if oracle['pairs'] else 'UNKNOWN',
                             pairs=oracle['pairs'][:1]))
    # Exact independent combinatorial checks guard against schedule truncation.
    assert enumerate_schedules(events(Case('one')))['schedules'] == 2
    assert enumerate_schedules(events(Case('three', counts=(3, 3))))['schedules'] == 20
    assert enumerate_schedules(events(Case('locked', locks=(0, 0))))['schedules'] == 2
    assert enumerate_schedules(events(Case('different', locks=(0, 1))))['schedules'] == 20
    late = enumerate_schedules(events(Case('late', counts=(3, 2), operations=('W', 'R'))))
    bad = [dict(verdict='UNKNOWN', pairs=[]),  # omitted candidate
           dict(verdict='FALSE', pairs=[]),  # omitted target
           dict(verdict='FALSE', pairs=[((0, 1, 'W', 0), (1, 2, 'R', 0))]),
           dict(verdict='FALSE', pairs=[((0, 3, 'W', 1), (1, 2, 'R', 0))]),
           dict(verdict='TIMEOUT', pairs=[]), dict(verdict='ERROR', pairs=[])]
    for observation in bad:
        try:
            compare(late, observation)
        except AssertionError:
            continue
        raise AssertionError('comparator accepted injected omission/corruption')
    safe = enumerate_schedules(events(Case('read-only', operations=('R', 'R'))))
    try:
        compare(safe, dict(verdict='FALSE', pairs=[((0, 1, 'R', 0), (1, 1, 'R', 0))]))
    except AssertionError:
        pass
    else:
        raise AssertionError('comparator accepted injected FALSE on oracle-safe input')
    totals['detected_injections'] = len(bad) + 1
    return dict(totals)


def mapped_targets(execution, access_lines):
    """Map actual executed helper loads/stores to independent event identities."""
    program = execution['program']
    traces = {trace['id']: trace for trace in execution['traces']}
    result = []
    for target in execution['targets']:
        trace = traces[target['trace']]
        assert trace['entry'] in ('worker0', 'worker1'), trace['entry']
        thread = int(trace['entry'][-1])
        access = access_lines[trace['entry']]
        accesses = []
        static_accesses = set()
        for index, step in enumerate(trace['steps']):
            inst = program['instructions'][step['statement']]
            op = {'load': 'R', 'store': 'W'}.get(inst['kind'])
            if op != access['operation'] or inst.get('line') != access['line']:
                continue
            address = step.get('encoded_address', {})
            obj = address.get('object', {})
            assert obj.get('id') in ('payload0', 'payload1', 'payload2'), obj
            assert address.get('width') == 4 and int(address.get('offset', '-1')) == 0, address
            static_accesses.add(step['statement'])
            accesses.append((index, (thread, len(accesses) + 1, op, int(obj['id'][-1]))))
        assert len(static_accesses) == 1, ('helper static target was cloned/replaced', static_accesses)
        found = [event for index, event in accesses if index == target['step']]
        assert len(found) == 1, ('missing actual helper access/target source mapping', target, accesses)
        result.append(found[0])
    assert len(result) == 2 and result[0][0] != result[1][0], result
    return tuple(sorted(result))


def run_case(case, bits, command, work, timeout):
    # The oracle has finished before any analyzer output is available.
    oracle = enumerate_schedules(events(case))
    code, mapping = source(case)
    stem = f'{case.name}-{bits}'
    source_path, report_path = work / (stem + '.c'), work / (stem + '.json')
    source_path.write_text(code)
    cmd = command + ['--analyses=race', '--concurrency=auto', '--partitioning=guards',
                     '--demote-race-to-unknown', f'-m{bits}', '--bmc-deadline-ms=20000',
                     '--bmc-solver-timeout-ms=1000', '--format=json',
                     '--report-file=' + str(report_path), '-o', str(work / (stem + '.db')),
                     str(source_path)]
    import os
    env = dict(os.environ, IKOS_BMC_RECOVER='1')
    env.pop('PYTHONPATH', None)
    env.pop('PYTHONHOME', None)
    record = dict(case=asdict(case), bits=bits, oracle=oracle, command=cmd,
                  source_sha256=hashlib.sha256(code.encode()).hexdigest(),
                  source_access_lines=mapping)
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, env=env, timeout=timeout)
        (work / (stem + '.log')).write_text(proc.stdout + proc.stderr)
        record['returncode'] = proc.returncode
        if proc.returncode != 0:
            observation = dict(verdict='ERROR', pairs=[])
        else:
            from analyzer.python.ikos.race_result import validate_race_result
            from validate_bmc_execution import check, digest_json
            report = json.loads(report_path.read_text())
            metadata = validate_race_result(report)
            executions = [r['info']['counterexample'] for r in report['reports']
                          if 'counterexample' in r['info']]
            pairs, validations = [], []
            for execution in executions:
                pair = mapped_targets(execution, mapping)
                certificate = dict(execution)
                program = certificate.pop('program')
                certificate['program_sha256'] = digest_json(program)
                checked = check(program, certificate)
                assert checked['status'] == 'ACCEPT', checked
                validations.append(dict(result=checked, mapped_pair=pair,
                                        program_sha256=digest_json(program),
                                        execution_sha256=digest_json(execution)))
                pairs.append(pair)
            observation = dict(verdict=metadata['verdict'], pairs=pairs,
                               ar_validations=validations, metadata=metadata)
    except subprocess.TimeoutExpired:
        observation = dict(verdict='TIMEOUT', pairs=[])
    except (AssertionError, KeyError, OSError, ValueError) as error:
        observation = dict(verdict='ERROR', pairs=[],
                           failure=f'{type(error).__name__}: {error}')
    record['observation'] = observation
    try:
        compare(oracle, observation)
        if oracle['pairs'] and observation['verdict'] == 'FALSE':
            # Mutate the real mapped observation, through the same comparison
            # used above, so an always-success test adapter cannot pass silently.
            missing_candidate = dict(observation, verdict='UNKNOWN', pairs=[])
            missing_target = dict(observation, pairs=[])
            wrong_pair = [list(event) for event in observation['pairs'][0]]
            wrong_pair[0][1] += 1
            wrong_identity = dict(observation, pairs=[wrong_pair])
            for injected in (missing_candidate, missing_target, wrong_identity):
                try:
                    compare(oracle, injected)
                except AssertionError:
                    continue
                raise AssertionError('comparison accepted a real-observation omission/corruption')
            record['real_observation_injections_rejected'] = 3
    except AssertionError as error:
        record.update(status='FAIL', failure=str(error))
    else:
        record['status'] = 'PASS'
    (work / (stem + '-comparison.json')).write_text(json.dumps(record, indent=2) + '\n')
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos', type=Path, default=ROOT / 'install/bin/ikos')
    parser.add_argument('--bits', type=int, choices=(32, 64), default=32)
    parser.add_argument('--case', action='append', help='exact case name, repeatable; default all 84')
    parser.add_argument('--output', type=Path, help='persist generated C, reports and coverage evidence')
    parser.add_argument('--timeout', type=int, default=35)
    parser.add_argument('--self-test', action='store_true', help='pure Python only; no IKOS process')
    args = parser.parse_args()
    summary = dict(oracle_self_test=self_test(), finite_scope=__doc__)
    if args.self_test:
        print(json.dumps(summary, indent=2))
        return
    sys.path.insert(0, str(ROOT))
    from ikos_runtime import runtime_command, runtime_identity, verify_runtime
    evidence_files = (Path(__file__).resolve(), ROOT / 'scripts/validate_bmc_execution.py')
    checker_identity = {str(path): hashlib.sha256(path.read_bytes()).hexdigest()
                        for path in evidence_files}
    runtime = runtime_identity(args.ikos.resolve())
    command = runtime_command(args.ikos.resolve())
    selected = [c for c in cases() if args.case is None or c.name in args.case]
    assert selected and (args.case is None or len(selected) == len(set(args.case))), 'unknown case selection'
    with tempfile.TemporaryDirectory(prefix='ikos-finite-coverage-') as temp:
        work = args.output.resolve() if args.output else Path(temp)
        work.mkdir(parents=True, exist_ok=True)
        assert not (work / 'comparison-summary.json').exists(), 'use a new evidence directory'
        records = []
        for case in selected:
            record = run_case(case, args.bits, command, work, args.timeout)
            records.append(record)
            print(f'{case.name}/{args.bits}: {record["status"]} '
                  f'{record["observation"]["verdict"]} '
                  f'{record["oracle"]["schedules"]} schedules', flush=True)
        verify_runtime(runtime)
        assert checker_identity == {str(path): hashlib.sha256(path.read_bytes()).hexdigest()
                                    for path in evidence_files}, 'oracle/checker changed during comparison'
        summary.update(runtime=runtime, checker_sha256=checker_identity,
                       bits=args.bits, selected_cases=len(records),
                       all_84_inputs_compared=len(records) == 84,
                       schedules=sum(r['oracle']['schedules'] for r in records),
                       results=[dict(case=r['case']['name'], status=r['status'],
                                     failure=r.get('failure')) for r in records])
        (work / 'comparison-summary.json').write_text(json.dumps(summary, indent=2) + '\n')
        assert all(r['status'] == 'PASS' for r in records), summary['results']


if __name__ == '__main__':
    main()
