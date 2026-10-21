"""Strict race-result protocol. Validation grants no execution semantics."""

SCHEMA = 1
POLICY = 'strict-coverage-v1'
SAFETY_CONTRACTS = {'coverage-readonly-heap-v1', 'coverage-bucket-ownership-v1'}
VERDICT_LINES = {'SAFE': 'The program is SAFE',
                 'FALSE': 'The program is definitely UNSAFE',
                 'UNKNOWN': 'The program is UNKNOWN'}


def validate_race_result(data, *, required=True, check_rows=True):
    """Validate property metadata independently of display/status filters.

    Legacy reports may be inspected with required=False; they cannot publish a
    result under this policy. Malformed new metadata is always an error.
    """
    if not isinstance(data, dict):
        raise ValueError('invalid race report')
    result = data.get('race_result')
    if result is None and not required and 'race_policy' not in data:
        return None
    if not isinstance(result, dict):
        raise ValueError('missing race_result (legacy or incomplete analysis)')
    if result.get('schema') != SCHEMA or type(result.get('schema')) is not int or result.get('policy') != POLICY:
        raise ValueError('unsupported race_result protocol')
    policy = data.get('race_policy', {'schema': SCHEMA, 'policy': POLICY})
    if (not isinstance(policy, dict) or type(policy.get('schema')) is not int
            or policy != {'schema': SCHEMA, 'policy': POLICY}):
        raise ValueError('race policy and result disagree')
    for key in ('analysis_complete', 'checks_complete', 'search_performed'):
        if type(result.get(key)) is not bool:
            raise ValueError('invalid race_result ' + key)
    for key in ('obligation_count', 'unresolved_count', 'safety_only_count', 'remaining_pairs'):
        if type(result.get(key)) is not int or result[key] < 0:
            raise ValueError('invalid race_result ' + key)
    obligations = result.get('obligations')
    if not isinstance(obligations, list) or len(obligations) != result['obligation_count']:
        raise ValueError('invalid coverage obligations')
    unresolved = safety_only = 0
    for item in obligations:
        if (not isinstance(item, dict) or not isinstance(item.get('source'), str)
                or not item['source'] or not isinstance(item.get('reason'), str) or not item['reason']
                or not isinstance(item.get('contract'), str) or type(item.get('safety_only')) is not bool
                or bool(item['contract']) != item['safety_only']):
            raise ValueError('invalid coverage obligation')
        unresolved += not item['contract']
        safety_only += item['safety_only']
        if item['contract'] and (item['source'] != 'effect' or item['contract'] not in SAFETY_CONTRACTS):
            raise ValueError('unrecognized safety coverage contract')
    if unresolved != result['unresolved_count'] or safety_only != result['safety_only_count']:
        raise ValueError('coverage counts disagree')
    coverage, verdict = result.get('coverage'), result.get('verdict')
    if coverage not in {'Pending', 'Blocked', 'Checked'} or verdict not in VERDICT_LINES:
        raise ValueError('invalid coverage state or verdict')
    complete = result['analysis_complete'] and result['checks_complete']
    if ((coverage == 'Checked' and (unresolved or not complete))
            or (coverage == 'Blocked' and not unresolved)
            or (coverage == 'Pending' and complete)
            or (verdict != 'UNKNOWN' and coverage != 'Checked')
            or (verdict == 'SAFE' and result['remaining_pairs'])
            or (verdict == 'FALSE' and (safety_only or not result['remaining_pairs'] or not result['search_performed']))):
        raise ValueError('race_result state contradicts verdict or search permission')
    if check_rows:
        rows = data.get('reports')
        if not isinstance(rows, list):
            raise ValueError('missing race reports')
        confirmed = 0
        for row in rows:
            if not isinstance(row, dict):
                raise ValueError('invalid race report row')
            info = row.get('info')
            if info is None and row.get('checker') != 17:
                continue
            if not isinstance(info, dict):
                raise ValueError('invalid race report row')
            bmc = info.get('bmc_result')
            if bmc is not None and not isinstance(bmc, dict):
                raise ValueError('invalid BMC result')
            selected = ('confirmed_pair_id' in info or 'counterexample' in info
                        or (bmc or {}).get('status') == 'CONFIRMED')
            error = row.get('checker') == 17 and row.get('status') == 2
            if selected or error:
                execution = info.get('counterexample')
                traces = execution.get('traces') if isinstance(execution, dict) else None
                if (verdict != 'FALSE' or row.get('status') != 2
                        or (bmc or {}).get('status') != 'CONFIRMED'
                        or info.get('verdict') == 'unknown'
                        or type(info.get('confirmed_pair_id')) is not int or info['confirmed_pair_id'] < 0
                        or not isinstance(traces, list) or not traces
                        or not any(isinstance(t, dict) and isinstance(t.get('steps'), list) and t['steps'] for t in traces)
                        or not isinstance(info.get('access_a'), dict) or not isinstance(info.get('access_b'), dict)):
                    raise ValueError('confirmed evidence contradicts race_result')
                confirmed += 1
        if verdict == 'FALSE' and not confirmed:
            raise ValueError('FALSE without a nonempty confirmed execution')
    return result


def race_verdict_line(data):
    return VERDICT_LINES[validate_race_result(data)['verdict']]
