#!/usr/bin/env python3
"""Archive one auditable record for each original or newly introduced UNKNOWN."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re


GAPS = {
    'non_global_object': '动态堆/共享栈对象的实例身份、范围和生命周期尚未进入候选 BMC；不能把分配位置当成具体对象。',
    'points_to_not_singleton': 'AI may-alias 集合不是实际指针选择；需要具体指针值/对象来源编码，不能任选一个目标确认。',
    'thread_activation_unproven': '未建立包含所选实例的合法创建前缀；堆上的句柄、嵌套创建、startup join 或不支持的实际参数仍可能阻断。',
    'transitive_thread_order': '跨多级 creator 的激活及排序缺少具体实例链，需要额外线程/创建依赖而非函数名排序。',
    'unmodeled_sync': '执行前缀含未支持的原子内存操作；需要相应内存模型和 RMW/同步语义，不能当普通 load/store。',
    'call_summary': '前缀调用缺少可执行的确定 callee 或经核实的 API 摘要；原子 helper 还须保留完整 atomic 包围区间。',
    'prefix_statement': '前缀含未支持的 AR 操作，需要补齐其定义性、状态变化及排序语义。',
    'target_not_reached': '现有边界/调用解析未得到目标的合法前缀；可能在循环之后、间接 callee 或更深依赖中，不能猜测退出。',
    'atomic_prefix_open': '目标位于尚未结束的 verifier atomic 区间；需要完整区间和独立目标索引，不能删除闭合检查。',
    'prefix_address': '前缀访问/锁的具体地址或对象范围不可证；需要指针供值、动态对象或更一般的地址表达式。',
    'address_base': '目标同一具体对象不可证；不能省略同址约束。',
    'mutex_identity': '具体锁对象/地址未建立；may-lock 摘要不能作为取得该锁的执行证据。',
    'mutex_ownership': '前缀解锁的所有权不满足当前摘要；错误检查/递归锁等属性及返回值尚未建模。',
    'symbolic_mutex_nested': '符号锁嵌套的实际身份和所有权未建模；当前只支持一个 outstanding 符号锁。',
    'prefix_value_origin': '某个已执行语句的值来源或定义性不可证；不能用自由符号填补 load/实参/未知返回值。',
    'prefix_arithmetic': '已执行算术的取值或定义性尚不可编码；先补来源和机器运算前提。',
    'assume_value': '路径 guard 的实际来源未编码，可能依赖额外线程/指针/未支持操作；不能自由选择。',
    'assume_predicate': '指针或浮点比较等谓词尚未建模；需明确类型和对象语义。',
    'address_value': '符号下标的实际值来源未编码，不能把 may-offset 当实际地址。',
    'smt_unsat': '已尝试的有限路径/候选不满足查询；不是无界 SAFE，也不证明此任务无法实现。',
    'smt_unknown': 'SMT 在当前预算内未给出结论。',
    'offset_prefilter': '当前 may-offset 与宽度已排除这个候选；其他候选/实际指针来源仍须审查。',
    'mixed_atomic': '候选含不确定原子属性，尚不能交给当前普通访问查询。',
    'startup_read_origin': 'startup read 没有可证明的初始化/此前写来源；不能取未来写或自由值。',
    'value_non_integer_cast': '指针与整数等转换缺少真实来源及目标 ABI 契约。',
    'create_arguments': '创建成功的句柄对象、属性或参数未满足当前摘要。',
}


def write_json(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--before', type=Path, required=True)
    ap.add_argument('--after', type=Path, required=True)
    ap.add_argument('--reference', type=Path, action='append', required=True)
    ap.add_argument('--output', type=Path, required=True)
    args = ap.parse_args()
    before = {r['task']: r for r in json.loads(args.before.read_text())}
    after = {r['task']: r for r in json.loads(args.after.read_text())}
    assert before.keys() == after.keys(), 'paired task sets differ'
    references = {}
    for directory in args.reference:
        for r in json.loads((directory / 'results.json').read_text()):
            assert r['task'] not in references, 'duplicate reference task'
            references[r['task']] = dict(r, result_file=str(directory / r['artifacts'] / 'result.json'))
    tasks = sorted(k for k in before if before[k]['verdict'] == 'Unknown' or after[k]['verdict'] == 'Unknown')
    assert all(k in references for k in tasks), 'missing reference coverage'
    args.output.mkdir(parents=True, exist_ok=False)
    records = args.output / 'tasks'
    records.mkdir()
    rows = []
    for key in tasks:
        a, b, ref = before[key], after[key], references[key]
        report_file = args.after.parent / b['artifacts'] / 'report.json'
        examples, candidate_pairs = [], []
        report_error = None
        if report_file.exists():
            try:
                report_data = json.loads(report_file.read_text())
            except ValueError as exc:
                report_error = str(exc)
                report_data = {}
            for report in report_data.get('reports', []):
                info = report.get('info', {})
                candidate_pairs.extend(info.get('conflicting_accesses', []))
                for candidate in info.get('bmc', {}).get('candidates', []):
                    examples.extend(candidate.get('examples', []))
        seen, evidence = set(), []
        for example in examples:
            identity = (example.get('reason'), example.get('file'), example.get('line'))
            if identity in seen or example.get('reason') not in GAPS:
                continue
            seen.add(identity)
            item = dict(example)
            source = Path(item.get('file', ''))
            line = item.get('line')
            if source.is_file() and isinstance(line, int):
                text = source.read_text(errors='replace').splitlines()
                item['source_context'] = '\n'.join(f'{i + 1}: {text[i]}' for i in range(max(0, line - 3), min(len(text), line + 2)))
            evidence.append(item)
            if len(evidence) >= 12:
                break
        source = Path(b['input'])
        for pair in candidate_pairs[:8]:
            for side in ('access_a', 'access_b'):
                access = pair[side]
                filename = Path(access.get('file', ''))
                if not filename.is_absolute(): filename = source.parent / filename
                line = access.get('line')
                if filename.is_file() and isinstance(line, int):
                    text = filename.read_text(errors='replace').splitlines()
                    access['source_context'] = '\n'.join(f'{i + 1}: {text[i]}' for i in range(max(0, line - 3), min(len(text), line + 2)))
        text = source.read_text(errors='replace').splitlines()
        anchors = []
        for index, line in enumerate(text):
            if re.search(r'\b(pthread_create|pthread_join|malloc|calloc|free|__VERIFIER_atomic_\w+|__sync_\w+|__atomic_\w+|pthread_(cond|barrier|spin)\w*|sem_\w+)\s*\(', line) and not re.search(r'\bextern\b', line):
                anchors.append(dict(line=index + 1, text=line.strip()))
        gaps = [GAPS[reason] for reason in b['counts'] if reason in GAPS]
        successes = {'ai_candidate_bounds', 'ai_no_wrap', 'ai_single_execution', 'integer_operation', 'sc_read_from',
                     'startup_read_from', 'selected_activation_occurrences', 'startup_create_offset',
                     'startup_literal_loop_hint',
                     'sat', 'not_searched_after_confirmation'}
        blockers = {k: v for k, v in b['counts'].items() if k not in successes}
        for reason in b['counts']:
            if 'budget' in reason:
                gaps.append(f'{reason}：现有搜索/展开资源耗尽；没有证明剩余行为无关。')
        disagreement = ((ref['reference'] == 'FAIL' and b['expected'] == 'Safe') or
                        (ref['reference'] == 'PASS' and b['expected'] == 'Race Found'))
        if disagreement:
            status = 'reference_label_disagreement'
        elif b['verdict'] == 'Race Found':
            assert ref['reference'] == 'FAIL' and b['expected'] == 'Race Found', 'new race lacks reference confirmation'
            status = 'restored_tp'
        elif ref['reference'] == 'FAIL':
            status = 'confirmed_reference_race_ikos_remaining'
        else:
            status = 'reference_' + ref['reference'].lower()
        row = dict(task=key, expected=b['expected'], data_model=b['data_model'],
                   original_unknown=a['verdict'] == 'Unknown', ikos_before=a['verdict'],
                   ikos_after=b['verdict'], dartagnan=ref['reference'], status=status,
                   source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
                   reference_result=ref['result_file'], ikos_result=str(report_file.with_name('result.json')),
                   ikos_report_parse_error=report_error,
                   observed_counts=b['counts'], observed_blockers=blockers, remaining_obligations=gaps,
                   candidate_access_pairs=candidate_pairs[:8],
                   blocker_source_evidence=evidence, input_source_anchors=anchors,
                   attempt=('完整复测本轮通用扩展：有界循环/重复创建、路径定义上下文、startup 实际读来源、'
                            '符号锁和符号地址 SC 供值、执行前缀同步检查、实际参数 callback、'
                            '提前终止不支持的展开分支；同时修正两项上游覆盖遗漏。'
                            if ref['reference'] == 'FAIL' and not disagreement else
                            '未获可直接采用的参考竞争确认；保留参考结论/失败与 IKOS 完整复测结果。'),
                   note='阻塞原因可能重叠且存在遮挡；本轮保留不等于 IKOS 数学上不可能实现。外部 FAIL 不等于原 SMT 模型独立重放。')
        filename = Path(key).stem + '-' + hashlib.sha256(key.encode()).hexdigest()[:12] + '.json'
        write_json(records / filename, row)
        row['record'] = 'tasks/' + filename
        rows.append(row)
    write_json(args.output / 'results.json', rows)
    write_json(args.output / 'summary.json', dict(tasks=len(rows), original_unknown=sum(r['original_unknown'] for r in rows),
                                               statuses=dict(Counter(r['status'] for r in rows)),
                                               remaining_blockers=dict(Counter(reason for r in rows if r['status'] == 'confirmed_reference_race_ikos_remaining' for reason in r['observed_blockers']))))
    lines = ['task\texpected\tbefore\tafter\tdartagnan\tstatus\trecord']
    lines += ['\t'.join(str(r[k]) for k in ('task', 'expected', 'ikos_before', 'ikos_after', 'dartagnan', 'status', 'record')) for r in rows]
    (args.output / 'results.tsv').write_text('\n'.join(lines) + '\n')
    print((args.output / 'summary.json').read_text())


if __name__ == '__main__':
    main()
