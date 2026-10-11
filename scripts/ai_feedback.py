#!/usr/bin/env python3
"""Experimental driver support: one frontend run, fresh analyzer process/DB per round."""
import contextlib
import copy
import hashlib
import json
import os
from pathlib import Path
import resource
import sys
import time


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


@contextlib.contextmanager
def logged(path):
    sys.stdout.flush()
    sys.stderr.flush()
    saved = os.dup(1), os.dup(2)
    with open(path, 'w') as log:
        try:
            os.dup2(log.fileno(), 1)
            os.dup2(log.fileno(), 2)
            yield
        finally:
            sys.stdout.flush()
            sys.stderr.flush()
            os.dup2(saved[0], 1)
            os.dup2(saved[1], 2)
            os.close(saved[0])
            os.close(saved[1])


class SharedIR:
    def __init__(self, source, output, prefix, bits, defines, timeout, binary=None):
        self.source, self.output = source.resolve(), output.resolve()
        self.output.mkdir(parents=True, exist_ok=False)
        prefix = prefix.resolve()
        packages = prefix / 'libexec/lib' / ('python%d.%d' % sys.version_info[:2]) / 'site-packages'
        if not (packages/'ikos').is_dir():
            raise RuntimeError('No installed IKOS Python package at ' + str(packages))
        sys.path.insert(0, str(packages))
        from ikos import analyzer, colors, log, report, settings
        from ikos.output_db import OutputDatabase
        self.api, self.report, self.database = analyzer, report, OutputDatabase
        self.binary = binary.resolve() if binary else Path(settings.ikos_analyzer())
        settings.ikos_analyzer = lambda: str(self.binary)
        self.binary_hash, self.source_hash = digest(self.binary), digest(self.source)
        self.defines = list(defines)
        colors.ENABLE = False
        log.setup('error')
        # All analysis/compiler defaults come from the installed frontend parser.
        argv = ['--analyses=race', '--concurrency=auto', '--demote-race-to-unknown',
                '--color=no', '-m', str(bits), '--cpu', str(max(1, int(timeout)))]
        argv += ['-D'+value for value in defines] + [str(self.source)]
        self.options = analyzer.parse_arguments(argv)
        self.options.log_level = 'error'
        self.options.cpu = timeout
        self.prefix, self.bits = prefix, bits
        self.ld_path = str(prefix/'lib') + ':' + str(prefix/'llvm/lib')
        if os.environ.get('LD_LIBRARY_PATH'):
            self.ld_path += ':' + os.environ['LD_LIBRARY_PATH']
        self.timeout = timeout
        self.pp = self.output/'input.pp.bc'
        before = resource.getrusage(resource.RUSAGE_CHILDREN)
        start = time.monotonic()
        with self.environment(None, False), logged(self.output/'frontend.log'):
            source_bc = self.source
            if analyzer.path_ext(str(self.source)) in analyzer.c_extensions + analyzer.cpp_extensions:
                source_bc = self.output/'input.bc'
                opt = self.options
                analyzer.clang(str(source_bc), str(self.source), opt.compiler_include_flags,
                               opt.compiler_define_flags, opt.compiler_warning_flags,
                               opt.compiler_disable_warnings, opt.compiler_machine_flags, False)
            analyzer.ikos_pp(str(self.pp), str(source_bc), self.options.entry_points,
                             self.options.opt_level, self.options.inline_all,
                             not self.options.no_bc_verify, True)
        after = resource.getrusage(resource.RUSAGE_CHILDREN)
        self.frontend = dict(wall_seconds=time.monotonic()-start,
                            cpu_seconds=after.ru_utime+after.ru_stime-before.ru_utime-before.ru_stime)
        self.pp_hash = digest(self.pp)
        self.write_metadata()

    @contextlib.contextmanager
    def environment(self, mode, bmc):
        changes = {'LD_LIBRARY_PATH': self.ld_path, 'IKOS_AI_PAIR_EXCLUSION': mode,
                   'IKOS_AI_PAIR_EXCLUSION_REPORT': None,
                   'IKOS_BMC_RECOVER': '1' if bmc else None}
        saved = {key: os.environ.get(key) for key in changes}
        try:
            for key, value in changes.items():
                if value is None:
                    os.environ.pop(key, None)
                else:
                    os.environ[key] = value
            yield
        finally:
            for key, value in saved.items():
                if value is None:
                    os.environ.pop(key, None)
                else:
                    os.environ[key] = value

    def intact(self):
        return (digest(self.source) == self.source_hash and digest(self.pp) == self.pp_hash
                and digest(self.binary) == self.binary_hash)

    def write_metadata(self):
        metadata = dict(source=str(self.source), source_sha256=self.source_hash,
                        pp_bc=str(self.pp), pp_bc_sha256=self.pp_hash,
                        analyzer=str(self.binary), analyzer_sha256=self.binary_hash,
                        data_model=self.bits, defines=self.defines, frontend=self.frontend,
                        freshness='Every round calls ikos_analyzer once, spawning a new process and new DB.',
                        timeout_scope='Existing frontend --cpu timer sends SIGALRM; not a hard OS CPU quota.')
        (self.output/'metadata.json').write_text(json.dumps(metadata, indent=2)+'\n')

    def run(self, name, domain, mode, bmc=False):
        if not self.intact():
            raise RuntimeError('Source, pp.bc, or analyzer changed between rounds')
        folder = self.output/name
        folder.mkdir()
        opts = copy.copy(self.options)
        opts.domain = domain
        database = folder/'output.db'
        opts.output_db = str(database)
        started = time.monotonic()
        before = resource.getrusage(resource.RUSAGE_CHILDREN)
        code = 0
        with self.environment(mode, bmc), logged(folder/'run.log'):
            if mode:
                os.environ['IKOS_AI_PAIR_EXCLUSION_REPORT'] = str(folder/'exclusions.jsonl')
            try:
                self.api.ikos_analyzer(str(database), str(self.pp), opts)
            except self.api.AnalyzerError as error:
                code = error.returncode
        after = resource.getrusage(resource.RUSAGE_CHILDREN)
        elapsed = time.monotonic()-started
        verdict, counts = 'NONE', None
        if code == 0 and self.intact():
            db = self.database(path=str(database))
            try:
                summary = self.report.generate_summary(db)
                counts = dict(ok=summary.ok, error=summary.error, warning=summary.warning,
                              unknown=summary.unknown, unreachable=summary.unreachable)
                if summary.error:
                    verdict = 'The program is definitely UNSAFE'
                elif summary.unknown:
                    verdict = 'The program is UNKNOWN'
                elif summary.warning:
                    verdict = 'The program is potentially UNSAFE'
                else:
                    verdict = 'The program is SAFE'
                rep = self.report.generate_report(db, status_filter=opts.status_filter,
                                                  analyses_filter=None)
                with open(folder/'report.json','w') as stream:
                    self.report.formats['json'](stream, opts.report_verbosity).format(rep)
            finally:
                db.close()
        if not self.intact():
            raise RuntimeError('Source, pp.bc, or analyzer changed during a round')
        evidence = folder/'exclusions.jsonl'
        records = [json.loads(line) for line in evidence.read_text().splitlines()] if evidence.exists() else []
        result = dict(name=name, domain=domain, mode=mode, bmc=bmc, returncode=code,
                      verdict=verdict, counts=counts, records=records,
                      pp_bc_sha256=self.pp_hash, database=str(database),
                      wall_seconds=elapsed,
                      child_cpu_seconds=after.ru_utime+after.ru_stime-before.ru_utime-before.ru_stime,
                      cumulative_child_peak_rss_kib=after.ru_maxrss)
        (folder/'summary.json').write_text(json.dumps(result,indent=2)+'\n')
        return result


def refinement_hint(records):
    # An interior representative of an interval hull may occupy a hole that a
    # congruence domain can retain. This is a heuristic about missing precision,
    # never a constraint, proof, path filter, or reason to discard the model.
    for row in records:
        if row.get('engine') != 'smt' or row.get('status') != 'SAT':
            continue
        model = row.get('abstract_model', {})
        for side in ('a', 'b'):
            fact = row[side]
            start = int(model['start_' + side])
            lo, hi = int(fact['lo']), int(fact['hi'])
            if int(fact['modulus']) == 1 and lo < start < hi:
                return dict(reason='abstract model uses an interior point of a non-singleton interval hull',
                            precision='interval-congruence', side=side,
                            access_id=fact['access_id'], model_start=str(start),
                            lo=str(lo), hi=str(hi), facts_and_model_used=row)
    return None


def main(args):
    from pathlib import Path
    if args.timeout <= 0:
        raise ValueError('timeout must be positive')
    start = time.monotonic()
    ir = SharedIR(args.source, args.output, args.prefix, args.bits,
                  args.define, args.timeout, args.analyzer)
    ir.options.cpu = max(0.001, args.timeout - (time.monotonic() - start))
    baseline = ir.run('initial', 'interval', '1', args.bmc)
    rounds, hint = [baseline], None
    verdict = baseline['verdict']
    if baseline['returncode'] == 0 and verdict in (
            'The program is potentially UNSAFE', 'The program is UNKNOWN'):
        hint = refinement_hint(baseline['records'])
        if hint is not None and time.monotonic() - start < args.timeout:
            ir.options.cpu = max(0.001, args.timeout - (time.monotonic() - start))
            refined = ir.run('refined', 'interval-congruence', '1', args.bmc)
            rounds.append(refined)
            if refined['returncode'] == 0 and refined['verdict'] != 'NONE':
                verdict = refined['verdict']
    result = dict(contract='query-model-domain-retry-shared-ir-v1',
                  source=str(ir.source), source_sha256=ir.source_hash,
                  analyzer_sha256=ir.binary_hash, pp_bc_sha256=ir.pp_hash,
                  data_model=args.bits, bmc_enabled=args.bmc,
                  frontend=ir.frontend, rounds=rounds, refinement_hint=hint,
                  final_verdict=verdict, wall_seconds=time.monotonic()-start,
                  support='Model only selects a numerical domain. Fresh AI convergence determines every verdict.',
                  not_claimed='No spurious-execution proof, injected invariant, CEGAR predicate, or BMC execution feedback.')
    (ir.output/'summary.json').write_text(json.dumps(result,indent=2)+'\n')
    print(verdict)
    print('AI rounds:', len(rounds))
    print('Evidence:', ir.output/'summary.json')
    return baseline['returncode'] if verdict == 'NONE' else 0


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('--prefix', type=Path, default=Path(__file__).resolve().parents[1]/'install')
    parser.add_argument('--analyzer', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--bits', type=int, choices=(32,64), default=32)
    parser.add_argument('--timeout', type=float, default=30)
    parser.add_argument('--bmc', action='store_true')
    parser.add_argument('--define', action='append', default=[])
    sys.exit(main(parser.parse_args()))
