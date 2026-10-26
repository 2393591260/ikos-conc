#!/usr/bin/env python3
"""Independent finite successful-create/provider-join language for Stage7 only.

This oracle is separate from Stage3's frozen84-input claim. Memory is SC; main
creates one provider, joins its real completion, reads ready, and conditionally
creates A/B. Each worker has1–3 retained memory actions and a Return. A normal
mutex section contains exactly one memory action and is one commutation class.
No production IR, candidate selector, unroller, encoder, or SMT is imported.
"""
import copy


def provider_oracle(a_count, b_count, ready_writes=(1,), common_mutex=False,
                    blocked_provider=False):
    assert a_count in (1, 2, 3) and b_count in (1, 2, 3)
    # Actor0=main,1=provider,2=A,3=B. State is a concrete small-step machine.
    initial = dict(pc={0: 0}, vc={0: [0, 0, 0, 0]}, done=set(), ready=0,
                   observed=None, history=[], mutex=[0, 0, 0, 0], races=set())
    totals = dict(complete_schedules=0, deadends=0, racy_schedules=0)
    target_pairs = set()

    def leq(a, b):
        return all(x <= y for x, y in zip(a, b))

    def data(s, tid, cell, write, ordinal):
        clock = tuple(s["vc"][tid])
        for other, old_cell, old_write, old_clock, old_ordinal in s["history"]:
            if (other != tid and old_cell == cell and (write or old_write)
                    and not leq(old_clock, clock) and not leq(clock, old_clock)):
                s["races"].add((other, old_ordinal, tid, ordinal, cell))
        s["history"].append((tid, cell, write, clock, ordinal))

    def dfs(s):
        if len(s["done"]) == len(s["pc"]):
            totals["complete_schedules"] += 1
            if s["races"]:
                totals["racy_schedules"] += 1
                target_pairs.update(s["races"])
            return
        successors = []
        for tid in sorted(s["pc"]):
            if tid in s["done"]:
                continue
            pc = s["pc"][tid]
            if tid == 0 and pc in (1, 5, 6):
                awaited = {1: 1, 5: 2, 6: 3}[pc]
                if awaited not in s["done"]:
                    continue
            if tid == 1 and blocked_provider and pc == len(ready_writes):
                continue
            n = copy.deepcopy(s)
            n["vc"][tid][tid] += 1
            n["pc"][tid] += 1
            if tid == 0:
                if pc in (0, 3, 4):
                    if pc == 3 and n["observed"] != 1:
                        n["pc"][0] = 7
                    else:
                        child = {0: 1, 3: 2, 4: 3}[pc]
                        assert child not in n["pc"]
                        n["pc"][child] = 0
                        n["vc"][child] = list(n["vc"][0])
                elif pc in (1, 5, 6):
                    child = {1: 1, 5: 2, 6: 3}[pc]
                    n["vc"][0] = [max(a, b) for a, b in
                                   zip(n["vc"][0], n["vc"][child])]
                elif pc == 2:
                    data(n, 0, "ready", False, 0)
                    n["observed"] = n["ready"]
                else:
                    assert pc == 7
                    n["done"].add(0)
            elif tid == 1:
                if pc < len(ready_writes):
                    data(n, 1, "ready", True, 0)
                    n["ready"] = ready_writes[pc]
                else:
                    n["done"].add(1)
            else:
                count = a_count if tid == 2 else b_count
                if pc == count:
                    n["done"].add(tid)
                else:
                    assert pc < count
                    # All earlier retained occurrences access different cells.
                    cell = 0 if pc + 1 == count else (1 if tid == 2 else 2)
                    if common_mutex:
                        n["vc"][tid] = [max(a, b) for a, b in
                                       zip(n["vc"][tid], n["mutex"])]
                        n["vc"][tid][tid] += 1
                    data(n, tid, ("payload", cell), tid == 2, pc + 1)
                    if common_mutex:
                        n["vc"][tid][tid] += 1
                        n["mutex"] = list(n["vc"][tid])
            successors.append(n)
        if not successors:
            totals["deadends"] += 1
        for successor in successors:
            dfs(successor)

    dfs(initial)
    expected = bool(totals["racy_schedules"])
    payload = [pair for pair in target_pairs if pair[-1] == ("payload", 0)]
    if expected:
        assert payload and all(
            {pair[0]: pair[1], pair[2]: pair[3]} == {2: a_count, 3: b_count}
            for pair in payload), payload
    return dict(totals, has_race=expected,
                target_pairs=sorted(map(str, target_pairs)),
                boundary="finite language with successful direct creates and exact provider join")


if __name__ == "__main__":
    for counts in ((2, 3), (3, 2), (3, 3)):
        positive = provider_oracle(*counts)
        protected = provider_oracle(*counts, common_mutex=True)
        stale = provider_oracle(*counts, ready_writes=(1, 0))
        blocked = provider_oracle(*counts, blocked_provider=True)
        assert positive["has_race"] and not protected["has_race"]
        assert not stale["has_race"] and not blocked["has_race"]
        print(counts, positive, protected, stale, blocked)
