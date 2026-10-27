#!/usr/bin/env python3
"""Offline recursive depth/return/VC controls; --ikos records unaccepted diagnostics."""
import argparse
import copy
import json
import os
from pathlib import Path
import subprocess
import signal
import sys
import tempfile
from types import SimpleNamespace
from validate_bmc_execution import Frame, Replay, Reject, Thread, Unsupported, check
ROOT = Path(__file__).resolve().parents[1]


def direct_contract(bits):
    width, word = (24, 4) if bits == 32 else (40, 8)
    replay = Replay.__new__(Replay)
    replay.native_sc = SimpleNamespace(groups={})
    replay.program = dict(pointer_bits=bits, little_endian=True, target_triple='i386-pc-linux-gnu' if bits == 32 else 'x86_64-pc-linux-gnu')
    replay.recursive_mutex_profile = True
    replay.atomic_owner = None
    replay.semaphores, replay.barriers, replay.created, replay.accesses, replay.writers = {}, {}, {}, {}, {}
    replay.locks, replay.attributes, replay.traces = {}, {}, {}
    replay.initial_main_frame = 1
    scalar = dict(kind='integer', bits=32, signed=True, size='4', extent='4')
    pointer = dict(kind='pointer', bits=bits, size=str(word), extent=str(word))
    attrtype = dict(kind='struct', size='4', extent='4', abi_alignment=4, fields=[])
    muttype = dict(kind='struct', size=str(width), extent=str(width), abi_alignment=word, fields=[])
    native = dict(extent=width, store_size=width, type_alignment=word, alignment=word, address_space=0,
                  declaration=False, thread_local=False, constant=False, externally_initialized=False, module_inline_asm=False)
    replay.program['globals'] = {'mutex': dict(frontend_type_name='union.pthread_mutex_t', declaration=False, native_mutex_object=native)}
    replay.objects = {
        'global:mutex': dict(size=width, live=True, variable='mutex', thread_local=False, alignment=word, type=muttype),
        'stack:1:attr': dict(size=4, live=True, variable='attr', thread_local=False, alignment=4, type=attrtype,
            owner_frame=1, owner_function='main', owner_thread=0, count=1, native_count=1,
            native_entry_allocation=True, dynamic_stack=False, native_alignment=4, native_address_space=0,
            native_owner='main', native_layout=dict(kind='struct', name='union.pthread_mutexattr_t', extent=4, store_size=4, alignment=4))}
    replay.memory = {'global:mutex': [0]*width, 'stack:1:attr': [None]*4}
    replay.functions = {'main': dict(parameters=[])}
    frame = Frame('main', 'entry', 1, env={'attr': ('stack:1:attr', 0)})
    parent = Thread(0, [frame], {0: 0}); other = Thread(1, [], {1: 0})
    attr, mutex = ('stack:1:attr', 0), ('global:mutex', 0)
    def api(name, args, result=0, recursive=False, before=0, after=0, who=parent, exact=True):
        count = len(args); types = [scalar if name.endswith('settype') and i == 1 else pointer for i in range(count)]
        values = [dict(kind='integer', value=str(v), type=scalar) if isinstance(v, int) else
                  dict(kind='global', id='mutex', type=pointer) if v == mutex else
                  dict(kind='local', id='attr', type=pointer) for v in args]
        source = name
        inst = dict(calling_convention=0, argument_layouts=[dict(size=width if v == mutex else 4, alignment=word if v == mutex else 4) for v in args],
            operands=[{}]+[dict(v, type=t) for v,t in zip(values,types)], result=dict(type=scalar),
            native_mutex=dict(name=source, declaration=True, variadic=False, matching_signature=True,
                calling_convention=0, call_convention=0, operand_bundles=False, ordinary_abi=True,
                arguments=count, parameters=count, return_bits=32, result_bits=32, pointer_bits=bits,index_bits=bits,
                little_endian=True, parameter_integer_bits=[32 if t is scalar else 0 for t in types],
                parameter_address_spaces=[99 if t is scalar else 0 for t in types], argument_objects=values))
        if name.endswith('settype'): inst['native_mutex']['settype_value'] = args[1]
        replay.functions[name] = dict(declaration=True, variadic=False, parameter_types=types, return_type=scalar)
        step = dict(mutex_semantics=dict(result=result, recursive=recursive, depth_before=before, depth_after=after))
        if not exact:
            step = {}; inst.pop('native_mutex')
        if name in ('pthread_mutex_lock', 'pthread_mutex_unlock'):
            step['encoded_address'] = dict(object=dict(kind='global', id='mutex'), offset='0', width=str(width))
        who.vc[who.instance] = who.vc.get(who.instance,0)+1
        actual = replay.api(name, args, inst, step, 'startup', 0, who, frame)
        assert actual == result
        return inst, step
    checks=0
    api('pthread_mutexattr_init',[attr]); api('pthread_mutexattr_settype',[attr,1]); api('pthread_mutex_init',[mutex,attr],recursive=True)
    checks+=3
    # Attribute changes/destroy cannot rewrite the initialized mutex snapshot.
    api('pthread_mutexattr_settype',[attr,2]); api('pthread_mutexattr_settype',[attr,-1],22)
    assert replay.attributes[attr] == 2 and replay.locks[mutex]['kind'] == 1
    api('pthread_mutexattr_destroy',[attr]); checks+=3
    lock='pthread_mutex_lock'; unlock='pthread_mutex_unlock'
    for level in range(1,9): api(lock,[mutex],recursive=True,before=level-1,after=level)
    released=dict(replay.locks[mutex]['released'])
    for level in range(8,1,-1):
        api(unlock,[mutex],recursive=True,before=level,after=level-1)
        assert replay.locks[mutex]['released']==released
    api(unlock,[mutex],recursive=True,before=1,after=0)
    final=dict(replay.locks[mutex]['released']); assert final and not replay.locks[mutex]['owners']; checks+=16
    api(unlock,[mutex],1,True,0,0,other); assert replay.locks[mutex]['released']==final; checks+=1
    api(lock,[mutex],recursive=True,before=0,after=1,who=other)
    assert all(other.vc.get(k,0)>=v for k,v in final.items()); checks+=1
    api(unlock,[mutex],1,True,0,0,parent); assert replay.locks[mutex]['depth']==1; checks+=1
    # Direct UINT32 overflow boundary, without impractical source loops.
    replay.locks[mutex]['depth']=0xffffffff
    api(lock,[mutex],11,True,0xffffffff,0xffffffff,other)
    assert replay.locks[mutex]['depth']==0xffffffff; checks+=1
    def reject(action):
        try: action()
        except (Reject,Unsupported): return 1
        raise AssertionError('accepted corrupt recursive premise')
    before_block=copy.deepcopy(replay.locks[mutex])
    checks+=reject(lambda: api(lock,[mutex],recursive=True,before=0,after=1,who=parent))
    assert replay.locks[mutex] == before_block
    checks+=reject(lambda: replay.region(mutex,1))
    checks+=reject(lambda: replay.region(attr,1))
    for key,value in [('depth_before',3),('depth_after',0),('result',0),('recursive',False)]:
        bad=dict(result=11,recursive=True,depth_before=0xffffffff,depth_after=0xffffffff);bad[key]=value
        checks+=reject(lambda bad=bad: replay.mutex_semantics(dict(mutex_semantics=bad),11,True,0xffffffff,0xffffffff))
    original=copy.deepcopy(replay.objects['stack:1:attr'])
    for key,value in [('native_count',2),('owner_frame',2),('native_entry_allocation',False),('native_alignment',1)]:
        replay.objects['stack:1:attr']=dict(original,**{key:value})
        checks+=reject(lambda: replay.recursive_attribute_object(attr))
    replay.objects['stack:1:attr']=original
    for key,value in [('store_size',width-1),('address_space',1),('externally_initialized',True),('type_alignment',1)]:
        replay.program['globals']['mutex']['native_mutex_object']=dict(native,**{key:value})
        checks+=reject(lambda: replay.recursive_global_mutex(mutex))
    replay.program['globals']['mutex']['native_mutex_object']=native
    inst,recorded=api(lock,[mutex],11,True,0xffffffff,0xffffffff,other)
    for field, value in [('object', dict(kind='global', id='another_mutex')), ('offset', '1'), ('width', str(width-1))]:
        damaged=copy.deepcopy(recorded);damaged['encoded_address'][field]=value
        checks+=reject(lambda damaged=damaged: replay.api(lock,[mutex],inst,damaged,'startup',0,other,frame))
    # A main Return ends the process; pthread_exit only ends its calling thread.
    replay.threads={'child':other}; replay.recursive_main_returned=True
    checks+=reject(lambda: replay.execute('child',0,{}))
    replay.recursive_main_returned=False

    for key,value in [('declaration',False),('ordinary_abi',False),('matching_signature',False),
                      ('return_bits',64),('index_bits',bits//2),('operand_bundles',True),
                      ('parameter_address_spaces',[1]),('argument_objects',[dict(kind='local',id='attr')])]:
        damaged=copy.deepcopy(inst);damaged['native_mutex'][key]=value
        checks+=reject(lambda damaged=damaged: replay.recursive_mutex_abi(lock,damaged,[mutex],frame))
    replay.functions[lock]['parameter_types']=[dict(pointer,bits=bits//2)]
    checks+=reject(lambda: replay.recursive_mutex_abi(lock,inst,[mutex],frame))
    replay.functions[lock]['parameter_types']=[pointer]
    # Setter value must bind to the native literal, not a report-supplied kind.
    saved_created=replay.created;replay.created={}
    inst,_=api('pthread_mutexattr_init',[attr])
    inst,_=api('pthread_mutexattr_settype',[attr,1])
    damaged=copy.deepcopy(inst);damaged['native_mutex']['settype_value']=2
    checks+=reject(lambda: replay.recursive_mutex_abi('pthread_mutexattr_settype',damaged,[attr,1],frame))
    replay.created=saved_created
    # Legacy normal/ERRORCHECK paths do not require new native/literal metadata.
    replay.recursive_mutex_profile=False
    replay.attributes={}; replay.locks={}
    replay.objects['global:mutex'].pop('mutex_regions',None)
    replay.objects['stack:1:attr'].pop('attribute_regions',None)
    api('pthread_mutexattr_init',[attr],exact=False)
    api('pthread_mutexattr_settype',[attr,2],exact=False)
    api('pthread_mutex_init',[mutex,attr],exact=False)
    api(lock,[mutex],exact=False)
    checks+=reject(lambda: api(lock,[mutex],exact=False))
    api(unlock,[mutex],exact=False)
    checks+=reject(lambda: api(unlock,[mutex],exact=False))
    checks+=reject(lambda: api('pthread_mutexattr_settype',[attr,0],exact=False))
    checks+=reject(lambda: api('pthread_mutexattr_settype',[attr,1],exact=False))
    checks+=5
    return checks


def source_projection_contract():
    # Gate-only fixtures deliberately stop before source loading or analyzer use.
    sys.path.insert(0, str(ROOT))
    import svcomp_witness
    saved = svcomp_witness.validate_selected_execution
    checks = 0
    try:
        for profile, record, indirect, symbolic in ((False, False, False, False),
                (False, True, False, False), (True, False, False, False),
                (False, False, True, False), (False, False, False, True)):
            calls = []
            svcomp_witness.validate_selected_execution = lambda info, model: calls.append(model) or dict(status='UNSUPPORTED')
            step = dict(statement='setter')
            if record:
                step['mutex_semantics'] = dict(recursive=True)
            replay = dict(schema=1, program=dict(instructions={
                'setter': dict(kind='call', operands=[dict(kind='function', id='pthread_mutexattr_settype'),
                    dict(kind='local', id='attr'), dict(kind='integer', value='1')])}),
                traces=[dict(id='startup', entry='main', steps=[step])])
            if indirect:
                replay['program']['instructions']['setter']['operands'][0] = dict(kind='variable', id='setter_alias')
            if symbolic:
                replay['program']['instructions']['setter']['operands'][2] = dict(kind='variable', id='kind_value')
            if profile:
                replay['mutex_profile'] = 'gnu-x86-recursive-v1'
            try:
                svcomp_witness.build_witness('/unused-recursive.c', dict(counterexample=replay), 'ILP32')
            except ValueError as exc:
                assert 'recursive mutex projection' in str(exc), exc
            else:
                raise AssertionError('recursive source projection skipped independent replay')
            assert calls == ['ILP32']
            checks += 1
    finally:
        svcomp_witness.validate_selected_execution = saved
    return checks


def source_cases():
    base = r"""#define _GNU_SOURCE
#include <pthread.h>
int data; pthread_mutex_t mutex;
void *worker(void *arg) {
  (void)arg; pthread_mutex_lock(&mutex); pthread_mutex_unlock(&mutex); data++; return 0;
}
int main(void) {
  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr); pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&mutex, &attr);
  pthread_t thread; pthread_mutex_lock(&mutex);
  pthread_create(&thread, 0, worker, 0);
  pthread_mutex_lock(&mutex); pthread_mutex_unlock(&mutex);
  pthread_mutex_unlock(&mutex); data++;
  pthread_join(thread, 0); return 0;
}
"""
    return [('post-final-race',base,True),
            ('partial-still-held',base.replace('  pthread_mutex_unlock(&mutex); data++;','  data++; pthread_mutex_unlock(&mutex);'),False),
            ('normal-relock',base.replace('PTHREAD_MUTEX_RECURSIVE','PTHREAD_MUTEX_NORMAL'),False),
            ('wrong-attr-storage',base.replace('pthread_mutexattr_t attr;','unsigned char attr[4];').replace('&attr','(pthread_mutexattr_t *)attr'),False)]



def run_analyzer(command, env, timeout):
    process = subprocess.Popen(command, env=env, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, text=True, start_new_session=True)
    try:
        stdout, stderr = process.communicate(timeout=timeout)
    except BaseException:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        process.communicate()
        raise
    return subprocess.CompletedProcess(command, process.returncode, stdout, stderr)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--ikos',type=Path)
    parser.add_argument('--bits',type=int,choices=(32,64))
    parser.add_argument('--report',type=Path)
    args=parser.parse_args(); rows=[dict(source_projection_checks=source_projection_contract())]
    for bits in ([args.bits] if args.bits else [32,64]):
        rows.append(dict(bits=bits,direct_checks=direct_contract(bits)))
        if args.ikos:
            from ikos_runtime import runtime_command, runtime_identity, verify_runtime
            identity = runtime_identity(args.ikos)
            env = {k: v for k, v in os.environ.items()
                   if not k.startswith('IKOS_') and k not in ('PYTHONPATH', 'PYTHONHOME')}
            env['IKOS_BMC_RECOVER'] = '1'
            with tempfile.TemporaryDirectory(prefix='ikos-recursive-') as tmp:
                for name,program,expect in source_cases():
                    source=Path(tmp)/(name+'.c');db=Path(tmp)/(name+'.db');report=Path(tmp)/(name+'.json')
                    source.write_text(program)
                    command=runtime_command(args.ikos.resolve())+['--analyses=race','--concurrency=auto','--partitioning=guards',
                        '--demote-race-to-unknown','--bmc-deadline-ms=20000','-m'+str(bits),'-o',str(db),'--format=json','--report-file',str(report),str(source)]
                    proc=run_analyzer(command,env=env,timeout=60)
                    assert proc.returncode==0,(name,proc.stdout,proc.stderr)
                    data=json.loads(report.read_text());executions=[r['info'] for r in data['reports'] if r.get('info',{}).get('counterexample')]
                    # The BMC subset does not supply the AI initialization frame
                    # or LockedCreation depth theorem. Record actual coverage and
                    # verdict; missing pairs/executions cannot count as SAFE.
                    from sweep_bmc_execution import validate_report
                    replays=validate_report(data)
                    assert all(result['status']=='ACCEPT' for result in replays),(name,replays)
                    if expect: assert executions,(name,data)
                    else: assert not executions,(name,data)
                    rows.append(dict(bits=bits,name=name,source_racy=expect,
                        executions=len(executions),replay=replays,
                        diagnostic_report=data,acceptance=False))
            verify_runtime(identity)
    if args.report: args.report.write_text(json.dumps(rows,indent=2)+'\n')
    print(json.dumps(rows))
if __name__=='__main__': main()
