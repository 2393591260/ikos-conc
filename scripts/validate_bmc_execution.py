#!/usr/bin/env python3
"""Concrete AR-prefix replay, independent of IKOS BMC/Z3.

The program snapshot is a separate trusted input. The certificate supplies only
statement occurrences, clock order and values assigned by the selected SMT model.
ACCEPT is conditional on the exported AR/frontend and the stated API contracts;
this is not a C-source/witness-2.2 validator or a universal soundness proof.
"""
import argparse
from collections import Counter
from dataclasses import dataclass, field
import hashlib
import json
from pathlib import Path


class Reject(Exception):
    pass


class Unsupported(Exception):
    pass


UNDEFINED = object()
NONDET = {'__VERIFIER_nondet_' + n for n in
          ('bool', 'char', 'uchar', 'short', 'ushort', 'int', 'uint', 'long',
           'ulong', 'longlong', 'ulonglong', 'size_t')}
BIN = {i + start: op for start in (2, 15) for i, op in enumerate(
       ('add', 'sub', 'mul', 'div', 'rem', 'shl', 'lshr', 'ashr', 'and', 'or', 'xor'))}
CMP = {i + start: op for start in (2, 10) for i, op in enumerate(('eq', 'ne', 'gt', 'ge', 'lt', 'le'))}
LOCKS = {
    'pthread_mutex_lock': ('lock', False), 'ar.pthread.mutex.lock': ('lock', False),
    'pthread_mutex_unlock': ('unlock', False), 'ar.pthread.mutex.unlock': ('unlock', False),
    'pthread_rwlock_rdlock': ('lock', True), 'ar.pthread.rwlock.rdlock': ('lock', True),
    'pthread_rwlock_wrlock': ('lock', False), 'ar.pthread.rwlock.wrlock': ('lock', False),
    'pthread_rwlock_unlock': ('unlock', False), 'ar.pthread.rwlock.unlock': ('unlock', False),
}


def require(condition, reason):
    if not condition:
        raise Reject(reason)


def digest_json(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(',', ':')).encode()).hexdigest()


def integer(value, t):
    if not isinstance(value, int) or isinstance(value, bool) or t['kind'] != 'integer':
        raise Unsupported('non-integer/undefined operand')
    width = t['bits']
    if not isinstance(width, int) or not 1 <= width <= 256:
        raise Unsupported('integer width')
    mask = (1 << width) - 1
    value &= mask
    return value - (1 << width) if t['signed'] and value & (1 << (width - 1)) else value


@dataclass
class Frame:
    function: str
    block: str
    uid: int
    pc: int = 0
    env: dict = field(default_factory=dict)
    pending: tuple | None = None
    once_pending: dict | None = None


@dataclass
class Thread:
    instance: int
    frames: list
    vc: dict
    finished: bool = False


class Replay:
    def __init__(self, program, certificate):
        require(certificate['schema'] == 1, 'certificate schema')
        require(certificate['program_sha256'] == digest_json(program), 'program snapshot hash mismatch')
        self.program, self.certificate = program, certificate
        self.functions, self.instructions = program['functions'], program['instructions']
        self.traces = {t['id']: t for t in certificate['traces']}
        require(len(self.traces) == len(certificate['traces']), 'duplicate trace identity')
        require({'a', 'b'} <= set(self.traces) and all(
            name in {'startup', 'main', 'provider', 'a', 'b'} or
            (name.startswith('aux_') and name[4:].isdigit())
            for name in self.traces), 'trace identities')
        # Timed provider profiles retain a real main root. These names do not
        # replace the checks below for unique main, actual create and completion.
        if 'main' in self.traces:
            require(self.traces['main']['entry'] == 'main' and
                    'startup' not in self.traces, 'timed main trace identity')
            require(all(self.traces[name]['entry'] != 'main' for name in ('a', 'b')),
                    'timed target entry identity')
        if 'provider' in self.traces:
            require('main' in self.traces and
                    self.traces['provider']['entry'] != 'main', 'provider trace identity')
        bindings = set()
        for name, trace in self.traces.items():
            if trace['entry'] == 'main':
                require('creator_trace' not in trace and 'creator_step' not in trace, 'main cannot have a creator')
                continue
            creator, step = trace.get('creator_trace'), trace.get('creator_step')
            require(creator in self.traces and type(step) is int and step >= 0, 'dynamic creation occurrence')
            require((creator, step) not in bindings, 'two traces share one creation occurrence')
            bindings.add((creator, step))
        self.threads, self.memory, self.objects = {}, {}, {}
        self.created, self.locks, self.accesses = {}, {}, {}
        self.next_instance, self.next_frame = 1, 0
        self.heap_counts = {}
        self.atomic_owner = None
        self.atomic_sections = []
        self.pending_markers = {}
        self.writers = {}
        self.attributes, self.conditions = {}, {}
        self.semaphores = {}
        self.stdout_vc = {}
        self.once_states = {}
        self.once_profile = any("once_role" in s for t in self.traces.values() for s in t["steps"])
        for name, glob in program['globals'].items():
            if glob['declaration'] or glob['type']['kind'] == 'unsupported':
                continue
            obj = 'global:' + name
            self.objects[obj] = dict(size=int(glob['type']['extent']), type=glob['type'], live=True,
                                     variable=name, thread_local=glob['thread_local'],
                                     alignment=glob.get('alignment'))
            self.memory[obj] = [None] * min(self.objects[obj]['size'], 1_000_000)
            if self.objects[obj]['size'] > 1_000_000:
                self.memory[obj] = []
            elif glob.get('frontend_function_initializer') is not None and glob['type']['kind']=='pointer':
                name=glob['frontend_function_initializer']
                require(name in self.functions, 'unknown function initializer')
                self.store((obj,0), ('function',name), glob['type'])
            elif glob['simple_initializer'] and len(glob['initializers']) == 1:
                self.initialize((obj, 0), glob['initializers'][0])

    def frame(self, name, args):
        fn = self.functions[name]
        if fn['declaration']:
            raise Unsupported('entry function declaration')
        require(len(args) >= len(fn['parameters']) if fn.get('variadic', False) else
                len(fn['parameters']) == len(args), 'actual/formal argument count')
        self.next_frame += 1
        f = Frame(name, fn['entry'], self.next_frame)
        for parameter, argument in zip(fn['parameters'], args):
            f.env[parameter['id']] = argument
        return f

    def value(self, operand, frame):
        k = operand['kind']
        if k == 'integer': return int(operand['value'])
        if k == 'global': return ('global:' + operand['id'], 0)
        if k == 'function': return ('function', operand['id'])
        if k == 'null': return ('null', 0)
        if k in ('variable', 'local'):
            return frame.env.get(operand['id'], UNDEFINED)
        if k == 'undef': return UNDEFINED
        raise Unsupported('operand kind ' + k)

    def region(self, pointer, size, alignment=0, opaque=False):
        require(isinstance(pointer, tuple) and len(pointer) == 2, 'invalid concrete pointer')
        obj, offset = pointer
        if obj in self.semaphores and not opaque:
            raise Unsupported('raw access to opaque semaphore storage')
        if obj not in self.objects:
            raise Unsupported('unmodeled object ' + str(obj))
        meta = self.objects[obj]
        require(meta['live'], 'access after object lifetime')
        require(isinstance(offset, int) and 0 <= offset and offset + size <= meta['size'], 'out-of-object access')
        require(not alignment or offset % alignment == 0, 'misaligned access')
        if not opaque and size > 0:
            require(not any(offset < start + width and start < offset + size
                            for start, width in meta.get('mutex_regions', [])),
                    'raw access to opaque stack mutex storage')
        if len(self.memory[obj]) != meta['size'] or meta['thread_local']:
            raise Unsupported('large/thread-local object')
        return obj, offset

    def stack_mutex(self, pointer):
        require(self.verified_pthread_abi(), 'stack mutex GNU x86 ABI')
        bits = self.program['pointer_bits']
        width, alignment = (24, 4) if bits == 32 else (40, 8)
        obj, offset = self.region(pointer, width, alignment, opaque=True)
        meta = self.objects[obj]
        require(meta.get('owner_frame') == getattr(self, 'initial_main_frame', None) and
                meta.get('owner_function') == 'main' and meta.get('owner_thread') == 0 and
                meta.get('count') == 1 and not self.functions['main']['parameters'],
                'stack mutex initial main allocation')
        require(meta.get('alignment', 0) >= alignment and meta.get('native_alignment', 0) >= alignment and
                meta.get('native_address_space') == 0 and meta.get('native_owner') == 'main',
                'stack mutex native allocation')
        node = meta.get('native_layout', {})
        require(int(node.get('extent', -1)) == meta['size'], 'stack mutex native extent')
        relative = offset
        for _ in range(32):
            if node.get('kind') == 'struct':
                if node.get('name') == 'union.pthread_mutex_t':
                    require(relative == 0 and int(node['extent']) == width and node['alignment'] == alignment,
                            'stack mutex declared leaf')
                    break
                fields = [f for f in node.get('fields', []) if int(f['offset']) <= relative <
                          int(f['offset']) + int(f['type'].get('extent', 0))]
                require(len(fields) == 1, 'stack mutex field identity')
                relative -= int(fields[0]['offset']); node = fields[0]['type']
            elif node.get('kind') == 'array':
                element = node.get('element', {}); stride = int(element.get('extent', 0))
                require(stride > 0 and 0 <= relative // stride < int(node.get('count', 0)),
                        'stack mutex array identity')
                relative %= stride; node = element
            else:
                raise Unsupported('stack mutex declared object type')
        else:
            raise Unsupported('stack mutex layout depth')
        require((offset, width) in meta.get('mutex_regions', []) and pointer in self.locks and
                self.locks[pointer].get('kind') == 0, 'stack mutex explicit default initialization')

    def pointer_shift(self, inst, values):
        pointer = values[0]
        if isinstance(pointer, tuple) and pointer[0] == 'absolute':
            raise Unsupported('numeric pointer arithmetic')
        require(isinstance(pointer, tuple) and pointer[0] not in ('null', 'function'), 'pointer shift base')
        require(len(inst['factors']) == len(values) - 1, 'pointer shift arity')
        require(all(isinstance(v, int) for v in values[1:]), 'pointer shift index')
        offset, delta = pointer[1], 0
        if inst.get('inbounds') is not True or inst.get('index_bits') != self.program['pointer_bits'] or inst.get('address_space') != 0:
            raise Unsupported('unverified object GEP contract')
        self.region(pointer, 0, opaque=True)
        native_terms = inst.get('native_terms')
        if not isinstance(native_terms, list) or len(native_terms) != len(inst['factors']):
            raise Unsupported('missing native GEP terms')
        bits = inst['index_bits']
        low, high = -(1 << (bits - 1)), (1 << (bits - 1)) - 1
        for position, (factor, value) in enumerate(zip(inst['factors'], values[1:]), 1):
            product = int(factor) * value
            delta += product
            term = native_terms[position - 1]
            operand = inst['operands'][position]
            if term.get('kind') == 'index':
                require(int(term['stride']) == int(factor) and
                        term['bits'] == operand['type']['bits'], 'native GEP stride/index width')
            elif term.get('kind') == 'field':
                require(int(factor) == 1 and operand.get('kind') == 'integer' and
                        value == int(term['offset']), 'native GEP field offset')
            else:
                raise Unsupported('native GEP term kind')
            width = min(bits, operand['type']['bits'])
            require(-(1 << (width - 1)) <= value < (1 << (width - 1)), 'GEP index truncation/sign')
            require(low <= product <= high and low <= delta <= high, 'GEP signed offset overflow')
            self.region((pointer[0], offset + delta), 0, opaque=True)
        return (pointer[0], offset + delta)

    def store(self, pointer, value, t, alignment=0):
        size = int(t['size']); obj, offset = self.region(pointer, size, alignment)
        if t['kind'] == 'integer':
            n = integer(value, t) % (1 << t['bits'])
            data = list(n.to_bytes(size, 'little' if self.program['little_endian'] else 'big'))
        elif t['kind'] == 'pointer' and isinstance(value, tuple):
            data = [('pointer-byte', value, i) for i in range(size)]
        elif value is UNDEFINED:
            data = [None] * size
        else:
            raise Unsupported('store value/type')
        self.memory[obj][offset:offset + size] = data
        for i in range(offset, offset + size):
            self.writers[(obj, i)] = getattr(self, 'current_occurrence', 'initial')

    def load(self, pointer, t, alignment=0):
        size = int(t['size']); obj, offset = self.region(pointer, size, alignment)
        data = self.memory[obj][offset:offset + size]
        if any(v is None for v in data): raise Unsupported('read of uninitialized bytes')
        if t['kind'] == 'integer' and all(isinstance(v, int) for v in data):
            return integer(int.from_bytes(bytes(data), 'little' if self.program['little_endian'] else 'big'), t)
        if t['kind'] == 'pointer' and all(isinstance(v, tuple) and v[0] == 'pointer-byte' for v in data):
            p = data[0][1]
            require(data == [('pointer-byte', p, i) for i in range(size)], 'mixed pointer representation')
            return p
        raise Unsupported('load representation/type')

    def initialize(self, pointer, value):
        kind, t = value['kind'], value['type']; obj, offset = pointer
        if obj.startswith('global:') and t['kind'] == 'struct' and kind in ('struct', 'zero'):
            covered = set()
            for field in t['fields']:
                covered.update(range(int(field['offset']), int(field['offset']) + int(field['type']['extent'])))
            for byte in range(int(t['extent'])):
                if byte not in covered: self.memory[obj][offset + byte] = 0
        if kind == 'integer': self.store(pointer, int(value['value']), t)
        elif kind == 'null': self.store(pointer, ('null', 0), t)
        elif kind == 'function': self.store(pointer, ('function', value['id']), t)
        elif kind == 'undef': pass
        elif kind == 'zero':
            if t['kind'] == 'integer': self.store(pointer, 0, t)
            elif t['kind'] == 'pointer': self.store(pointer, ('null', 0), t)
            elif t['kind'] == 'array':
                e = t['element']
                for i in range(int(t['count'])):
                    self.initialize((obj, offset + i * int(e['extent'])), dict(kind='zero', type=e))
            elif t['kind'] == 'struct':
                for field in t['fields']:
                    self.initialize((obj, offset + int(field['offset'])), dict(kind='zero', type=field['type']))
        elif kind == 'sequence':
            for i, element in enumerate(value['values']):
                self.initialize((obj, offset + i * int(t['element']['extent'])), element)
        elif kind == 'struct':
            for field in value['fields']:
                self.initialize((obj, offset + int(field['offset'])), field['value'])
        # Other initializer forms remain uninitialized; any use is unsupported.

    def advance(self, frame, statement):
        blocks = self.functions[frame.function]['blocks']
        block = blocks[frame.block]
        if frame.pc == len(block['statements']):
            wanted = self.instructions[statement]['block']
            todo, visited = list(block['successors']), set()
            while todo:
                candidate = todo.pop()
                if candidate in visited: continue
                visited.add(candidate)
                if candidate == wanted: break
                if not blocks[candidate]['statements']: todo.extend(blocks[candidate]['successors'])
            else: raise Reject('CFG transition skips instructions or guards')
            frame.block, frame.pc = wanted, 0
        expected = blocks[frame.block]['statements']
        require(frame.pc < len(expected) and expected[frame.pc] == statement, 'wrong function/statement occurrence')
        frame.pc += 1

    @staticmethod
    def join_vc(a, b):
        for key, value in b.items(): a[key] = max(a.get(key, 0), value)

    def assign(self, frame, instruction, step, result):
        if 'result' not in instruction: return
        if 'encoded_result' in step:
            require(isinstance(result, int) and result == int(step['encoded_result']), 'SMT value differs from concrete replay')
        if 'encoded_pointer' in step:
            pointer = step['encoded_pointer']
            require(pointer.get('kind') in ('function','null'), 'malformed SMT pointer identity')
            expected = ('function', pointer['id']) if pointer['kind']=='function' else ('null',0)
            require(result == expected, 'SMT pointer identity differs from concrete replay')
        if 'encoded_function' in step:
            require(result == ('function', step['encoded_function']), 'SMT function identity differs from concrete replay')
        if 'encoded_object_pointer' in step:
            self.region(result, 0)
            self.encoded_address({'encoded_address': step['encoded_object_pointer']}, result, 0)
        frame.env[instruction['result']['id']] = result

    def arithmetic(self, inst, values):
        t = inst['result']['type']; a, b = values
        integer(a, t); integer(b, t)
        op = BIN.get(inst['operator'])
        if op is None: raise Unsupported('arithmetic operator')
        if op == 'add': r = a + b
        elif op == 'sub': r = a - b
        elif op == 'mul': r = a * b
        elif op in ('div', 'rem'):
            require(b != 0, 'division by zero')
            require(not (t['signed'] and a == -(1 << (t['bits'] - 1)) and b == -1), 'signed division overflow')
            q = abs(a) // abs(b) * (-1 if (a < 0) != (b < 0) else 1)
            r = q if op == 'div' else a - q * b
            if inst['exact']: require(a == q * b, 'inexact division')
        elif op in ('shl', 'lshr', 'ashr'):
            require(0 <= b < t['bits'], 'invalid shift count')
            u = a % (1 << t['bits'])
            signed_a = u - (1 << t['bits']) if u & (1 << (t['bits'] - 1)) else u
            r = a << b if op == 'shl' else signed_a >> b if op == 'ashr' else u >> b
            if op == 'shl' and t['signed']:
                require(a >= 0 and r < (1 << (t['bits'] - 1)), 'signed left shift undefined')
            if inst['exact'] and op != 'shl': require(u & ((1 << b) - 1) == 0, 'inexact right shift')
        elif op == 'and': r = a & b
        elif op == 'or': r = a | b
        else: r = a ^ b
        result = integer(r, t)
        if inst['no_wrap']: require(result == r, 'no-wrap overflow')
        return result

    def encoded_address(self, step, pointer, width):
        if 'encoded_address' not in step: return
        address = step['encoded_address']; obj = address['object']
        if obj['kind'] == 'global': require(pointer[0] == 'global:' + obj['id'], 'SMT global object mismatch')
        elif obj['kind'] == 'local': require(self.objects[pointer[0]]['variable'] == obj['id'], 'SMT stack object mismatch')
        elif obj['kind'] == 'heap':
            require(self.objects[pointer[0]].get('allocation') == obj['allocation'] and
                    self.objects[pointer[0]].get('owner') == obj['owner'] and
                    self.objects[pointer[0]].get('occurrence') == obj['occurrence'], 'SMT allocation instance mismatch')
            require(self.objects[pointer[0]]['size'] == int(obj['extent']), 'SMT allocation extent mismatch')
            # Legacy occurrence-one evidence may omit the extra event binding.
            # Replay still owns identity; later ordinals require the actual step.
            if 'allocation_event' in obj:
                require(self.objects[pointer[0]].get('allocation_event') ==
                        obj['allocation_event'], 'SMT allocation event mismatch')
            else:
                require(obj['occurrence'] == 1, 'missing dynamic allocation event')
        else: raise Unsupported('encoded object')
        require(pointer[1] == int(address['offset']), 'SMT byte offset mismatch')
        require(width == int(address['width']), 'SMT access width mismatch')

    def library_layout(self, inst, argument, size, alignment=None):
        if not self.verified_pthread_abi(): raise Unsupported('unverified pthread ABI')
        if inst.get('calling_convention') != 0: raise Unsupported('library calling convention')
        layouts = inst.get('argument_layouts', [])
        if argument >= len(layouts) or layouts[argument].get('size') != size or (
                alignment is not None and layouts[argument].get('alignment') != alignment):
            raise Unsupported('library argument layout')

    def verified_pthread_abi(self):
        target = self.program.get('target_triple', '')
        bits = self.program['pointer_bits']
        return target.endswith('linux-gnu') and (
            (bits == 32 and target.startswith(('i386-', 'i486-', 'i586-', 'i686-'))) or
            (bits == 64 and target.startswith('x86_64-')))

    def thread_output_subobject(self, declared, offset, width):
        kind = declared.get('kind')
        if kind == 'integer':
            return (offset == 0 and declared.get('bits') == self.program['pointer_bits'] and
                    int(declared['size']) == width and int(declared['extent']) == width)
        if kind == 'array':
            stride = int(declared['element']['extent'])
            return (stride > 0 and 0 <= offset < int(declared['count']) * stride and
                    self.thread_output_subobject(declared['element'], offset % stride, width))
        if kind == 'struct':
            return any(int(field['offset']) <= offset < int(field['offset']) + int(field['type']['extent']) and
                       self.thread_output_subobject(field['type'], offset - int(field['offset']), width)
                       for field in declared['fields'])
        return False

    def condition(self, pointer):
        obj, offset = self.region(pointer, 48)
        if pointer not in self.conditions:
            if not obj.startswith('global:') or any(b != 0 for b in self.memory[obj][offset:offset+48]):
                raise Unsupported('condition initialization')
            self.conditions[pointer] = dict(mutex=None)
        return self.conditions[pointer]

    def marker(self, trace_id, statement, name, pointer=None):
        self.pending_markers.setdefault(trace_id, []).append((statement, name, pointer))

    def atomic_begin(self, thread):
        require(self.atomic_owner is None, 'nested atomic')
        self.atomic_owner = thread.instance
        self.atomic_sections.append(dict(begin=dict(thread.vc), end=None))

    def atomic_end(self, thread):
        require(self.atomic_owner == thread.instance, 'unowned atomic end')
        self.atomic_sections[-1]['end'] = dict(thread.vc)
        self.atomic_owner = None

    def api(self, name, args, inst, step, trace_id, index, thread, frame):
        fn = self.functions[name]
        if inst.get('native_intrinsic') == 'llvm.stacksave':
            require(name == 'ar.stacksave' and fn['declaration'] and not args and
                    fn['return_type']['kind'] == 'pointer', 'stack save intrinsic')
            # Opaque token: no modeled object, dereference or restore capability.
            return ('stack-save-token', frame.uid)
        if name == 'ar.memset':
            native = inst.get('native_memset', {})
            types = fn.get('parameter_types', [])
            require(inst.get('native_intrinsic') == 'llvm.memset' and fn['declaration'] and
                    not fn['variadic'] and fn['return_type']['kind'] == 'unsupported' and
                    'result' not in inst and len(args) == len(types) == 5 and
                    self.program['pointer_bits'] in (32, 64), 'private zero intrinsic')
            require(all(op['type'] == t for op, t in zip(inst['operands'][1:], types)) and
                    types[0]['kind'] == 'pointer' and
                    all(t['kind'] == 'integer' for t in types[1:]),
                    'private zero call types')
            pointer, byte, length, alignment, volatile = args
            require(byte == 0 and volatile == 0 and type(length) is int and length > 0 and
                    native.get('byte') == byte and native.get('length') == length and
                    native.get('volatile') is False and native.get('address_space') == 0 and
                    native.get('alignment') == alignment and type(alignment) is int and
                    alignment > 0 and alignment & (alignment - 1) == 0,
                    'private zero operands')
            obj, offset = self.region(pointer, length, alignment)
            meta = self.objects[obj]
            require(offset == 0 and length == meta['size'] and meta.get('count') == 1 and
                    meta.get('owner_frame') == frame.uid and meta.get('owner_thread') == thread.instance and
                    native.get('destination', {}).get('id') == meta.get('variable') and
                    self.value(native['destination'], frame) == pointer,
                    'private zero allocation identity')
            array = meta.get('type', {}); source = meta.get('native_layout', {})
            element = array.get('element', {}); source_element = source.get('element', {})
            require(array.get('kind') == source.get('kind') == 'array' and
                    int(array['count']) == int(source['count']) > 0 and
                    element.get('kind') == 'integer' and element['bits'] % 8 == 0 and
                    source_element.get('bits') == element['bits'] and
                    int(element['size']) == int(element['extent']) == element['bits'] // 8 and
                    int(array['extent']) == int(source['extent']) == length ==
                    int(array['count']) * int(element['size']) and
                    meta.get('native_address_space') == 0 and
                    meta.get('native_owner') == frame.function and
                    meta.get('native_alignment') == meta.get('alignment'),
                    'private zero array layout')
            self.encoded_address(step, pointer, length)
            self.memory[obj][:] = [0] * length
            for i in range(length):
                self.writers[(obj, i)] = self.current_occurrence
            return UNDEFINED
        if self.atomic_owner is not None and not (
                fn.get('intrinsic_nondet') or name in NONDET or fn.get('intrinsic_assume') or
                name in ('__VERIFIER_assume', 'ffs', '__VERIFIER_atomic_begin', '__VERIFIER_atomic_end')):
            raise Unsupported('external API inside verifier atomic: ' + name)
        if name in ('sem_init', 'sem_wait', 'sem_post'):
            count = 3 if name == 'sem_init' else 1
            types = fn['parameter_types']
            bits = self.program['pointer_bits']
            extent, alignment = (16, 4) if bits == 32 else (32, 8)
            require(self.verified_pthread_abi() and fn['declaration'] and
                    inst.get('calling_convention') == 0 and
                    not fn['variadic'] and len(args) == len(types) == count and
                    types[0]['kind'] == 'pointer' and types[0]['bits'] == bits and
                    int(types[0]['size']) == bits // 8 and fn['return_type']['kind'] == 'integer' and
                    fn['return_type']['bits'] == 32 and fn['return_type']['signed'] and
                    int(fn['return_type']['size']) == 4, 'semaphore ABI')
            require(all(op['type'] == t for op, t in zip(inst['operands'][1:], types)) and
                    ('result' not in inst or inst['result']['type'] == fn['return_type']), 'semaphore call types')
            native = inst.get('native_semaphore', {})
            expected_bits = [0, 32, 32] if name == 'sem_init' else [0]
            require(self.program.get('little_endian') is True and
                    native.get('pointer_bits') == bits and native.get('little_endian') is True and
                    native.get('target_triple') == self.program.get('target_triple') and
                    native.get('name') == name and native.get('declaration') is True and
                    native.get('variadic') is False and native.get('calling_convention') == 0 and
                    native.get('matching_signature') is True and
                    native.get('operand_bundles') is False and
                    native.get('special_parameter_abi') is False and
                    native.get('parameter_bits') == native.get('argument_bits') == expected_bits and
                    native.get('parameter_address_space') == native.get('argument_address_space') == 0 and
                    native.get('return_bits') == native.get('result_bits') == 32,
                    'native semaphore ABI')
            self.library_layout(inst, 0, extent, alignment)
            require(inst['argument_layouts'][0].get('name') == 'union.sem_t', 'semaphore argument type')
            pointer = args[0]
            self.region(pointer, extent, alignment, opaque=True)
            require(pointer[0].startswith('global:') and pointer[1] == 0, 'semaphore identity')
            global_name = pointer[0][len('global:'):]
            glob = self.program['globals'][global_name]
            require(not glob['declaration'] and not glob['thread_local'] and
                    glob.get('frontend_type_name') == 'union.sem_t' and
                    int(glob['type']['size']) == int(glob['type']['extent']) == extent and
                    glob['alignment'] >= alignment, 'semaphore object')
            source_object = glob.get('native_semaphore', {})
            require(source_object.get('extent') == source_object.get('store_size') == extent and
                    source_object.get('type_alignment') == alignment and
                    source_object.get('alignment', 0) >= alignment and
                    source_object.get('address_space') == 0 and
                    source_object.get('declaration') is False and
                    source_object.get('thread_local') is False and
                    source_object.get('constant') is False and
                    source_object.get('externally_initialized') is False,
                    'native semaphore object')
            if 'result' in inst:
                result = inst['result']['id']
                require(not any(op.get('id') == result and op.get('kind') in ('variable', 'local')
                                for code in self.program['instructions'].values()
                                for op in code.get('operands', [])), 'used semaphore result')
            if name == 'sem_init':
                require(all(t['kind'] == 'integer' and t['bits'] == 32 for t in types[1:]) and
                        args[1] == 0 and type(args[2]) is int and 0 <= args[2] <= 2147483647 and
                        thread.instance == 0 and not self.created and
                        pointer[0] not in self.semaphores, 'semaphore initialization')
                self.semaphores[pointer[0]] = dict(count=args[2], providers={})
            else:
                require(pointer[0] in self.semaphores, 'uninitialized semaphore')
                state = self.semaphores[pointer[0]]
                if name == 'sem_post':
                    require(state['count'] < 2147483647, 'semaphore overflow')
                    state['count'] += 1
                    self.join_vc(state['providers'], thread.vc)
                else:
                    require(state['count'] > 0, 'blocking semaphore wait')
                    state['count'] -= 1
                    # glibc acquire observes all earlier release providers.
                    self.join_vc(thread.vc, state['providers'])
            return 0
        if name == 'ffs':
            types = fn['parameter_types'] + [fn['return_type']]
            require(fn['declaration'] and not fn['variadic'] and len(args) == 1 and
                    len(fn['parameter_types']) == 1 and inst.get('calling_convention') == 0 and
                    all(t.get('kind') == 'integer' and t.get('bits') == 32 and
                        t.get('signed') and int(t.get('size', 0)) == 4 for t in types), 'ffs contract')
            require(inst['operands'][1]['type'] == types[0] and
                    ('result' not in inst or inst['result']['type'] == types[-1]), 'ffs call types')
            require(isinstance(args[0], int) and integer(args[0], types[0]) == args[0], 'ffs argument')
            bits = args[0] & 0xffffffff
            return (bits & -bits).bit_length()
        if fn.get('intrinsic_nondet') or name in NONDET:
            require(not args and not fn['parameter_types'] and fn['declaration'], 'nondet contract')
            t = inst['result']['type']
            require(t == fn['return_type'] and t['kind'] == 'integer', 'nondet result type')
            value = int(step.get('encoded_result', '0'))
            require(integer(value, t) == value and (name != '__VERIFIER_nondet_bool' or value in (0,1)), 'nondet range')
            return value
        if fn.get('intrinsic_assume') or name == '__VERIFIER_assume':
            require(len(args) == 1 and isinstance(args[0], int) and args[0] != 0, 'false assume'); return UNDEFINED
        if name in ('pthread_create', 'ar.pthread.create'):
            require(len(args) == 4 and args[1] == ('null',0) and args[2][0] == 'function', 'create arguments')
            width = self.program['pointer_bits']//8
            if self.program['pointer_bits'] not in (32, 64): raise Unsupported('thread handle ABI width')
            self.library_layout(inst, 0, width, width)
            t = inst['operands'][1]['type'].get('pointee')
            if (t is None or t['kind'] != 'integer' or t['bits'] != self.program['pointer_bits'] or
                    int(t['size']) != width): raise Unsupported('thread output handle type')
            obj, offset = self.region(args[0], width, width)
            if 'encoded_address' in step:
                address = step['encoded_address']
                require(address.get('alignment') == width, 'SMT thread output alignment mismatch')
                meta = self.objects[obj]
                base_alignment = meta.get('alignment')
                if base_alignment is None: raise Unsupported('thread output object alignment missing')
                require(isinstance(base_alignment, int) and not isinstance(base_alignment, bool) and
                        base_alignment >= width and base_alignment % width == 0,
                        'thread output object alignment')
                # Malloc storage is untyped; other objects must have a declared pthread_t subobject.
                if 'allocation' not in meta:
                    declared = meta.get('type', {})
                    stride = int(declared.get('extent', 0))
                    count = meta.get('count', 1)
                    if (stride <= 0 or not 0 <= offset // stride < count or
                            not self.thread_output_subobject(declared, offset % stride, width)):
                        raise Unsupported('thread output declared object type')
                self.encoded_address(step, args[0], width)
            elif 'allocation' in self.objects[obj]:
                raise Reject('missing encoded heap output address')
            target = args[2][1]; instance = self.next_instance; self.next_instance += 1
            self.store(args[0], instance, t, width)
            self.created[instance] = dict(function=target, argument=args[3], vc=dict(thread.vc),
                                          creator_trace=trace_id, creator_step=index,
                                          joined=False, detached=False)
            for child_id, child in self.traces.items():
                if child.get('creator_trace') == trace_id and child.get('creator_step') == index:
                    require(child['entry'] == target and child_id not in self.threads, 'dynamic creation binding')
                    self.threads[child_id] = Thread(instance, [self.frame(target, [args[3]])], dict(thread.vc))
            return 0
        if name in ('malloc', 'ar.libc.malloc'):
            native = inst.get('native_malloc', {})
            bits = self.program['pointer_bits']
            types, returned = fn['parameter_types'], fn['return_type']
            require(bits in (32, 64) and fn['declaration'] and not fn['variadic'] and
                    inst.get('calling_convention') == 0 and len(types) == 1 and
                    types[0].get('kind') == 'integer' and types[0].get('bits') == bits and
                    returned.get('kind') == 'pointer' and returned.get('bits') == bits and
                    len(inst['operands']) == 2 and inst['operands'][1]['type'] == types[0] and
                    inst.get('result', {}).get('type') == returned, 'malloc call types')
            require(native.get('name') == 'malloc' and native.get('declaration') is True and
                    native.get('variadic') is False and native.get('calling_convention') == 0 and
                    native.get('operand_bundles') is False and
                    native.get('matching_signature') is True and native.get('parameter_bits') == [bits] and
                    native.get('argument_bits') == [bits] and native.get('return_address_space') == 0 and
                    native.get('result_address_space') == 0, 'native malloc ABI')
            require(len(args) == 1 and isinstance(args[0], int) and 0 < args[0] <= 1_048_576, 'malloc extent')
            if thread.instance != 0 or len(thread.frames) != 1 or frame.function != 'main' or self.created:
                raise Unsupported('allocation outside pre-spawn main')
            key = (frame.uid, step['statement'])
            occurrence = self.heap_counts.get(key, 0) + 1
            self.heap_counts[key] = occurrence
            object_id = 'heap:' + str(frame.uid) + ':' + step['statement'] + ':' + str(occurrence)
            require(object_id not in self.objects, 'duplicate dynamic allocation identity')
            self.objects[object_id] = dict(size=args[0], live=True, variable=object_id, thread_local=False,
                allocation=step['statement'], owner='main', occurrence=occurrence,
                allocation_event=dict(trace=trace_id, step=index),
                alignment=self.program['pointer_bits']//8 if self.verified_pthread_abi() else None)
            self.memory[object_id] = [None] * args[0]
            return (object_id, 0)
        if name in ('free', 'ar.libc.free'):
            require(len(args) == 1, 'free argument count')
            pointer = args[0]
            if pointer == ('null', 0):
                require('encoded_address' not in step, 'unexpected encoded free address')
                return UNDEFINED
            require(isinstance(pointer, tuple) and len(pointer) == 2 and
                    pointer[0] in self.objects and 'allocation' in self.objects[pointer[0]] and
                    pointer[1] == 0, 'invalid free pointer')
            require(self.objects[pointer[0]]['live'], 'double free')
            if any(count > 1 for count in self.heap_counts.values()):
                require(thread.instance == 0 and len(thread.frames) == 1 and
                        frame.function == 'main' and not self.created,
                        'repeated allocation free outside pre-spawn main')
            require('encoded_address' in step, 'missing encoded free address')
            self.encoded_address(step, pointer, 0)
            self.objects[pointer[0]]['live'] = False
            return UNDEFINED
        if name == 'pthread_detach':
            types = fn['parameter_types']
            returned = fn['return_type']
            require(self.verified_pthread_abi() and fn['declaration'] and not fn['variadic'] and
                    len(args) == len(types) == 1 and inst.get('calling_convention') == 0 and
                    types[0].get('kind') == 'integer' and
                    types[0].get('bits') == self.program['pointer_bits'] and
                    int(types[0].get('size', 0)) == self.program['pointer_bits'] // 8 and
                    returned.get('kind') == 'integer' and returned.get('bits') == 32 and
                    returned.get('signed') and int(returned.get('size', 0)) == 4,
                    'detach ABI')
            require(len(inst.get('operands', [])) == 2 and inst['operands'][1]['type'] == types[0] and
                    ('result' not in inst or inst['result']['type'] == returned), 'detach call types')
            require(len(args)==1 and args[0] in self.created, 'unknown detach handle')
            created = self.created[args[0]]
            require(not created['joined'] and not created['detached'], 'instance not joinable')
            created['detached'] = True
            return 0
        if name in ('pthread_join', 'ar.pthread.join'):
            types, returned = fn['parameter_types'], fn['return_type']
            bits = self.program['pointer_bits']
            require(self.verified_pthread_abi() and fn['declaration'] and not fn['variadic'] and
                    len(args) == len(types) == 2 and inst.get('calling_convention') == 0 and
                    types[0].get('kind') == 'integer' and types[0].get('bits') == bits and
                    int(types[0].get('size', 0)) == bits // 8 and
                    types[1].get('kind') == 'pointer' and types[1].get('bits') == bits and
                    int(types[1].get('size', 0)) == bits // 8 and
                    returned.get('kind') == 'integer' and returned.get('bits') == 32 and
                    returned.get('signed') and int(returned.get('size', 0)) == 4,
                    'join ABI')
            require(len(inst.get('operands', [])) == 3 and
                    all(inst['operands'][i + 1]['type'] == t for i, t in enumerate(types)) and
                    ('result' not in inst or inst['result']['type'] == returned), 'join call types')
            require(len(args)==2 and args[0] in self.created, 'unknown join handle')
            require(args[1] == ('null',0), 'join result storage unsupported')
            created = self.created[args[0]]
            require(not created['joined'], 'instance joined twice')
            require(not created['detached'], 'instance detached before join')
            matched = [t for t in self.threads.values() if t.instance == args[0]]
            if not matched and 'joined_completion' in step:
                require(self.atomic_owner is None, 'auxiliary completion inside verifier atomic')
                completion = step['joined_completion']
                require(completion['creator_trace'] == created['creator_trace'] and
                        completion['creator_step'] == created['creator_step'], 'join creation occurrence')
                fn = self.functions[created['function']]
                require(not fn['declaration'] and len(fn['blocks']) == 1, 'auxiliary completion body')
                block = fn['blocks'][fn['entry']]
                require(not block['successors'] and block['statements'] == [completion['statement']],
                        'auxiliary completion CFG')
                ret = self.instructions[completion['statement']]
                require(ret['kind'] == 'return' and len(ret['operands']) == 1 and
                        ret['operands'][0]['kind'] == 'null', 'auxiliary actual return')
                if len(fn['parameters']) != 1 or fn['return_type']['kind'] != 'pointer' or fn.get('variadic'):
                    raise Unsupported('auxiliary entry signature')
                child = Thread(args[0], [], dict(created['vc']), finished=True)
                child.vc[args[0]] = child.vc.get(args[0],0) + 1
                self.threads['joined:' + str(args[0])] = child
                matched = [child]
            if 'joined_trace' in step:
                completed = step['joined_trace']
                child_id, last = completed['trace'], completed['step']
                require(child_id in self.traces and type(last) is int and
                        last == len(self.traces[child_id]['steps']) - 1, 'join terminal occurrence')
                child_trace = self.traces[child_id]
                require(child_trace.get('creator_trace') == created['creator_trace'] and
                        child_trace.get('creator_step') == created['creator_step'], 'join creation occurrence')
                require(child_id in self.threads and self.threads[child_id].instance == args[0] and
                        self.threads[child_id].finished, 'joined instance completion not replayed')
                ret = self.instructions[child_trace['steps'][last]['statement']]
                require(ret['kind'] == 'return', 'join terminal return')
            if not matched or not matched[0].finished: raise Unsupported('joined instance completion not replayed')
            self.join_vc(thread.vc, matched[0].vc); created['joined'] = True; return 0
        if name in LOCKS:
            rw = 'rwlock' in name
            width = (32 if self.program['pointer_bits']==32 else 56) if rw else (24 if self.program['pointer_bits']==32 else 40)
            self.library_layout(inst, 0, width, self.program['pointer_bits']//8 if not rw else None)
            require(len(args)==1, 'lock argument count')
            action, reader = LOCKS[name]; pointer = args[0]
            # Contract: glibc default static mutex/rwlock, target ILP32/LP64.
            rw = 'rwlock' in name
            width = (32 if self.program['pointer_bits']==32 else 56) if rw else (24 if self.program['pointer_bits']==32 else 40)
            local = isinstance(pointer, tuple) and str(pointer[0]).startswith('stack:')
            obj, offset = self.region(pointer, width, opaque=local)
            if obj.startswith('stack:'):
                if rw: raise Unsupported('stack rwlock')
                self.stack_mutex(pointer)
            pointee = inst['operands'][1]['type'].get('pointee')
            self.encoded_address(step, pointer, width)
            state = self.locks.setdefault(pointer, dict(owners={}, released={}, width=width))
            require(state['width'] == width, 'lock storage width changed')
            if action == 'lock':
                data = self.memory[obj][offset:offset+width]
                zero = lambda b: b == 0 or (isinstance(b, tuple) and b[:2] == ('pointer-byte', ('null', 0)))
                if any(b is not None and not zero(b) for b in data): raise Unsupported('nondefault mutex/rwlock initializer')
                if any(b is None for b in data): raise Unsupported('uninitialized lock storage')
                require(thread.instance not in state['owners'], 'recursive acquisition')
                require(not state['owners'] or reader and all(state['owners'].values()), 'overlapping exclusive lock ownership')
                self.join_vc(thread.vc, state['released']); state['owners'][thread.instance] = reader
            else:
                require(thread.instance in state['owners'], 'unlock without ownership')
                del state['owners'][thread.instance]; self.join_vc(state['released'], thread.vc)
            return 0
        if name in ('pthread_mutexattr_init', 'pthread_mutexattr_settype', 'pthread_mutexattr_destroy'):
            self.library_layout(inst, 0, 4, 4)
            self.region(args[0], 4, 4)
            if thread.instance != 0 or self.created: raise Unsupported('mutex attribute lifecycle after spawn')
            if name.endswith('_init'):
                require(args[0] not in self.attributes, 'reinitialized mutex attribute')
                self.attributes[args[0]] = 0
            else:
                require(args[0] in self.attributes, 'uninitialized mutex attribute')
                if name.endswith('_destroy'): del self.attributes[args[0]]
                else:
                    if args[1] != 2: raise Unsupported('mutex attribute kind')
                    self.attributes[args[0]] = 2
            return 0
        if name == 'pthread_mutex_init':
            require(len(args)==2, 'mutex initialization argument count')
            kind = 0
            if args[1] != ('null',0):
                self.library_layout(inst, 1, 4, 4)
                require(args[1] in self.attributes, 'mutex attribute lifetime')
                kind = self.attributes[args[1]]
                if kind != 2: raise Unsupported('nondefault mutex kind')
            width=24 if self.program['pointer_bits']==32 else 40
            self.library_layout(inst, 0, width, self.program['pointer_bits']//8)
            local = isinstance(args[0], tuple) and str(args[0][0]).startswith('stack:')
            obj, offset=self.region(args[0],width,opaque=local)
            if thread.instance != 0 or self.created:
                raise Unsupported('mutex initialization after spawn or outside main')
            require(not any(previous[0] == obj and previous[1] < offset + width and
                            offset < previous[1] + state['width']
                            for previous, state in self.locks.items()),
                    'reinitialized or previously used mutex storage')
            if obj.startswith('stack:'):
                require(not any(a['object'] == obj and a['offset'] < offset + width and
                                offset < a['offset'] + a['width'] for a in self.accesses.values()),
                        'raw stack mutex access before initialization')
                self.objects[obj].setdefault('mutex_regions', []).append((offset, width))
            self.memory[obj][offset:offset+width]=[0]*width
            self.locks[args[0]]=dict(owners={},released={},kind=kind,width=width); return 0
        if name in ('pthread_cond_init', 'pthread_cond_wait', 'pthread_cond_signal', 'pthread_cond_broadcast'):
            self.library_layout(inst, 0, 48)
            if name == 'pthread_cond_init':
                require(len(args)==2 and args[1]==('null',0) and args[0] not in self.conditions, 'condition initialization arguments/lifetime')
                obj, offset = self.region(args[0], 48)
                self.memory[obj][offset:offset+48] = [0]*48
                self.conditions[args[0]] = dict(mutex=None)
            else:
                condition = self.condition(args[0])
                if name == 'pthread_cond_wait':
                    width = 24 if self.program['pointer_bits']==32 else 40
                    self.library_layout(inst, 1, width)
                    require(len(args)==2 and args[1] in self.locks and thread.instance in self.locks[args[1]]['owners'], 'condition wait ownership')
                    require(condition['mutex'] in (None, args[1]), 'condition wait mutex binding')
                    condition['mutex'] = args[1]
                    # Choose a POSIX spurious wake. No signal HB is manufactured.
                    self.marker(trace_id, step['statement'], 'condition_release', args[1])
                    self.marker(trace_id, step['statement'], 'condition_reacquire', args[1])
            return 0
        if name in ('pthread_exit', 'ar.pthread.exit'):
            require(len(args)==1, 'thread exit argument count')
            thread.finished=True
            for active in thread.frames:
                for obj, meta in self.objects.items():
                    if obj.startswith('stack:'+str(active.uid)+':'): meta['live']=False
            thread.frames.clear()
            return UNDEFINED
        if name in ('printf', 'ar.libc.printf', 'puts', 'ar.libc.puts'):
            if not args: raise Unsupported('missing stdio format')
            obj, offset = self.region(args[0], 1)
            text=[]
            for byte in self.memory[obj][offset:]:
                if not isinstance(byte, int): raise Unsupported('stdio format representation')
                if byte == 0: break
                text.append(byte)
            else: raise Reject('unterminated stdio format')
            output_length = len(text)
            if 'printf' in name:
                output_length, cursor, argument = 0, 0, 1
                while cursor < len(text):
                    if text[cursor] != ord('%'):
                        output_length += 1; cursor += 1; continue
                    cursor += 1
                    require(cursor < len(text), 'incomplete stdio conversion')
                    conversion = chr(text[cursor]); cursor += 1
                    if conversion == '%': output_length += 1; continue
                    if conversion not in 'diuoxXc': raise Unsupported('stdio conversion')
                    require(argument < len(args), 'missing stdio argument')
                    t = inst['operands'][argument+1]['type']; value = args[argument]; argument += 1
                    if t['kind'] != 'integer' or t['bits'] != 32 or t['signed'] != (conversion in 'dic'):
                        raise Unsupported('stdio promoted argument type')
                    integer(value, t)
                    if conversion in 'diu': rendered = str(value)
                    elif conversion == 'o': rendered = format(value, 'o')
                    elif conversion in 'xX': rendered = format(value, conversion)
                    else: rendered = 'c'
                    output_length += len(rendered)
                require(argument == len(args), 'extra stdio arguments')
            else: require(len(args) == 1, 'puts argument count')
            # POSIX stream locking can create HB; retain it instead of claiming
            # an output-only call has no synchronization effects.
            self.join_vc(thread.vc, self.stdout_vc)
            self.join_vc(self.stdout_vc, thread.vc)
            return output_length if 'printf' in name else min(len(text) + 1, (1 << 31) - 1)
        if name in ('__VERIFIER_atomic_begin','__VERIFIER_atomic_end'):
            if name.endswith('begin'):
                self.atomic_begin(thread)
            else:
                self.atomic_end(thread)
            return UNDEFINED
        raise Unsupported('external API ' + name)

    def once_call(self, args, inst, step, trace_id, index, thread, frame):
        require(self.atomic_owner is None and frame.pending is None and frame.once_pending is None,
                'once invocation pending mode')
        if any(f.once_pending is not None for f in thread.frames):
            raise Unsupported('nested once callback')
        require('expanded_callee' not in step and 'encoded_result' not in step,
                'once callback is not the API callee/result')
        require(step.get('once_role') in ('active', 'passive') and
                type(step.get('once_call_step')) is int and step['once_call_step'] == index,
                'once actual call binding')
        require(len(args) == 2 and isinstance(args[1], tuple) and args[1][0] == 'function',
                'once callback identity')
        control, callback = args
        require(isinstance(control, tuple) and len(control) == 2 and
                isinstance(control[0], str) and control[0].startswith('global:') and control[1] == 0,
                'once global control identity')
        globname = control[0][7:]
        glob = self.program['globals'].get(globname, {})
        native = inst.get('native_once', {})
        expected = dict(declaration=True, variadic=False, calling_convention=0, call_convention=0,
                        matching_signature=True, operand_bundles=False, arguments=2, parameters=2,
                        return_bits=32, result_bits=32, special_parameter_abi=False,
                        control_bits=32, control_address_space=0, callback_parameter_void=True,
                        callback_parameter_count=0, callback_parameter_variadic=False,
                        callback_parameter_address_space=0, callback_signature_matches=True,
                        callback_declaration=False, callback_void=True, callback_count=0,
                        callback_variadic=False, callback_convention=0, control=globname, callback=callback[1])
        require(all(native.get(k) == v for k, v in expected.items()), 'once native API/callback signature')
        function = self.functions.get(callback[1], {})
        require(function.get('declaration') is False and function.get('parameters') == [] and
                function.get('native_void_return') is True and not function.get('variadic'),
                'once zero-argument callback frame')
        api = self.functions.get('pthread_once', {})
        rt = api.get('return_type', {})
        require(api.get('declaration') is True and rt.get('kind') == 'integer' and
                rt.get('bits') == 32 and rt.get('signed') is True and not api.get('variadic'),
                'once AR API signature')
        bits = self.program.get('pointer_bits')
        params = api.get('parameter_types', [])
        operands = inst.get('operands', [])
        def pointer_type(t, integer_pointee=False):
            return (t.get('kind') == 'pointer' and t.get('bits') == bits and
                    (not integer_pointee or
                     (t.get('pointee', {}).get('kind') == 'integer' and
                      t.get('pointee', {}).get('bits') == 32)))
        require(inst.get('calling_convention') == 0 and len(params) == 2 and
                pointer_type(params[0], True) and pointer_type(params[1]) and
                len(operands) == 3 and operands[0].get('kind') == 'function' and
                operands[0].get('id') == 'pthread_once' and
                operands[1].get('kind') == 'global' and operands[1].get('id') == globname and
                pointer_type(operands[1].get('type', {}), True) and
                operands[2].get('kind') == 'function' and operands[2].get('id') == callback[1] and
                pointer_type(operands[2].get('type', {})), 'once AR call convention/operand signature')
        result_type = inst.get('result', {}).get('type')
        if result_type is not None:
            require(result_type.get('kind') == 'integer' and result_type.get('bits') == 32 and
                    result_type.get('signed') is True, 'once AR result signature')
        facts = glob.get('native_once_control', {})
        expected_object = dict(zero_initializer=True, value_bits=32, address_space=0,
                               constant=False, externally_initialized=False,
                               module_inline_asm=False, module_aliases=False,
                               constructors=False, destructors=False, all_uses_direct=True)
        require(all(facts.get(k) == v for k, v in expected_object.items()) and
                glob.get('declaration') is False and glob.get('thread_local') is False and
                glob.get('alignment', 0) >= 4, 'once static native control closure')
        t = glob.get('type', {})
        initial = glob.get('initializers', [])
        require(t.get('kind') == 'integer' and t.get('bits') == 32 and int(t.get('size', 0)) == 4 and
                glob.get('simple_initializer') is True and len(initial) == 1 and
                initial[0].get('kind') in ('integer', 'zero') and
                (initial[0].get('kind') == 'zero' or int(initial[0].get('value', 1)) == 0),
                'once AR zero initializer')
        triple = self.program.get('target_triple', '')
        require('linux' in triple and 'gnu' in triple and
                (triple.startswith('x86_64') or triple.startswith(('i386', 'i486', 'i586', 'i686'))) and
                self.program.get('pointer_bits') in (32, 64) and self.program.get('little_endian') is True,
                'once native data model')
        # Cross-check the native-use manifest against complete AR instructions;
        # a raw write/copy/alias cannot be accepted by an exported seal boolean.
        uses = facts.get('uses', [])
        require(len(uses) == len(set(uses)), 'once duplicate source occurrence')
        actual_uses = set()
        for sid, instruction in self.instructions.items():
            operands = instruction.get('operands', [])
            references = [i for i, op in enumerate(operands)
                          if op.get('kind') == 'global' and op.get('id') == globname]
            if not references:
                continue
            require(instruction.get('kind') == 'call' and len(operands) == 3 and references == [1] and
                    operands[0].get('kind') == 'function' and operands[0].get('id') == 'pthread_once',
                    'once raw control access/alias')
            actual_uses.add(sid)
        require(set(uses) == actual_uses and step['statement'] in actual_uses,
                'once physical native/AR use manifest')
        state = self.once_states.setdefault(control, dict(phase='unstarted'))
        role = step['once_role']
        if role == 'active':
            require(state['phase'] == 'unstarted', 'once duplicate active initializer')
            state.update(phase='claimed', owner=(thread.instance, frame.uid))
        else:
            require(not (state.get('phase') in ('claimed', 'running') and
                         state.get('owner', (None,))[0] == thread.instance), 'recursive once')
        pending = dict(control=control, callback=callback[1], role=role, index=index,
                       statement=step['statement'], instruction=inst, entered=False, returned=False)
        frame.once_pending = pending
        self.marker(trace_id, step['statement'], 'once_enter' if role == 'active' else 'once_return', pending)

    def once_marker(self, name, pending, trace_id, index, step, thread):
        frame = thread.frames[-1]
        require(frame.once_pending is pending and step.get('once_call_step') == pending['index'] and
                step['statement'] == pending['statement'], 'once exact pending boundary')
        state = self.once_states[pending['control']]
        if name == 'once_enter':
            require(pending['role'] == 'active' and not pending['entered'] and
                    state['phase'] == 'claimed' and state['owner'] == (thread.instance, frame.uid),
                    'once callback enter state')
            callback = self.frame(pending['callback'], [])
            pending.update(entered=True, callback_frame=callback.uid)
            state.update(phase='running', owner=(thread.instance, callback.uid))
            thread.frames.append(callback)
        else:
            require(state['phase'] == 'done', 'once return before callback completion')
            if pending['role'] == 'active':
                require(pending['entered'] and pending['returned'] and
                        state['owner'] == (thread.instance, pending['callback_frame']), 'once active return owner')
            else:
                # Immutable winner clock; never join passive effects back into it.
                self.join_vc(thread.vc, state['completion'])
            require('encoded_result' not in step or
                    ('result' in pending['instruction'] and int(step['encoded_result']) == 0),
                    'nonzero successful once API result')
            self.assign(frame, pending['instruction'], step, 0)
            frame.once_pending = None

    def execute(self, trace_id, index, step):
        require(trace_id in self.threads, 'thread executes before actual create')
        thread = self.threads[trace_id]
        require(not thread.finished, 'execution after return')
        if self.once_profile:
            require(not any(t.finished and self.traces[n]['entry'] == 'main'
                            for n, t in self.threads.items()), 'once profile execution after main Return')
        require(self.atomic_owner in (None,thread.instance), 'interleaved verifier atomic block')
        thread.vc[thread.instance] = thread.vc.get(thread.instance,0)+1
        self.current_occurrence = dict(trace=trace_id, step=index)
        if 'synthetic_event' in step:
            pending = self.pending_markers.get(trace_id, [])
            require(pending and pending[0][:2] == (step['statement'], step['synthetic_event']), 'unjustified synthetic boundary')
            _, name, pointer = pending.pop(0)
            if not pending: del self.pending_markers[trace_id]
            if name in ('once_enter', 'once_return'):
                self.once_marker(name, pointer, trace_id, index, step, thread)
            elif name == 'atomic_end':
                self.atomic_end(thread)
            elif name == 'atomic_begin':
                self.atomic_begin(thread)
            elif name in ('condition_release', 'condition_reacquire'):
                state = self.locks[pointer]
                width = 24 if self.program['pointer_bits']==32 else 40
                self.encoded_address(step, pointer, width)
                if name == 'condition_release':
                    require(thread.instance in state['owners'], 'condition release ownership')
                    del state['owners'][thread.instance]
                    self.join_vc(state['released'], thread.vc)
                else:
                    require(not state['owners'], 'condition reacquire blocked')
                    self.join_vc(thread.vc, state['released'])
                    state['owners'][thread.instance] = False
            else: raise Unsupported('synthetic synchronization boundary')
            return
        require(trace_id not in self.pending_markers, 'missing synthetic boundary')
        frame = thread.frames[-1]; inst=self.instructions[step['statement']]
        self.advance(frame,step['statement'])
        kind=inst['kind']; operands=inst['operands']; values=[self.value(v,frame) for v in operands]
        if inst.get('abstract_initializer'):
            require('encoded_result' not in step, 'abstract initializer promoted to model value')
            self.assign(frame,inst,step,UNDEFINED); return
        result=UNDEFINED
        if kind=='allocate':
            count=values[0]; require(isinstance(count,int) and count>0, 'allocation count')
            obj='stack:'+str(frame.uid)+':'+inst['result']['id']; size=int(inst['allocated_type']['extent'])*count
            if size>1_000_000: raise Unsupported('large stack object')
            require(obj not in self.objects, 'repeated stack allocation identity')
            self.objects[obj]=dict(size=size,type=inst['allocated_type'],live=True,variable=inst['result']['id'],
                                   dynamic_stack=operands[0]['kind'] != 'integer',
                                   thread_local=False,alignment=inst.get('alignment'), count=count,
                                   owner_frame=frame.uid, owner_function=frame.function, owner_thread=thread.instance,
                                   **{k: inst[k] for k in ('native_layout', 'native_alignment',
                                      'native_address_space', 'native_owner') if k in inst})
            self.memory[obj]=[None]*size; result=(obj,0)
        elif kind=='assign': result=values[0]
        elif kind=='unary':
            op=inst['operator']; dst=inst['result']['type']; src=operands[0]['type']; value=values[0]
            if dst['kind']=='pointer' and src['kind']=='pointer' and op==14: result=value
            elif dst['kind']=='pointer' and src['kind']=='integer' and op in (12,13):
                width = self.program['pointer_bits']
                if width not in (32,64) or src['bits'] != width:
                    raise Unsupported('non-native integer pointer conversion')
                bits = integer(value,src) % (1 << src['bits']) % (1 << width)
                # Numeric pointers carry bits, never permission to access an object.
                result = ('absolute',bits) if bits else ('null',0)
            elif dst['kind']=='integer' and src['kind']=='pointer' and op in (10,11):
                if not isinstance(value,tuple) or value[0] not in ('absolute','null'):
                    raise Unsupported('object pointer integer representation')
                width = self.program['pointer_bits']
                if width not in (32,64) or dst['bits'] != width:
                    raise Unsupported('non-native pointer integer conversion')
                require(type(value[1]) is int and 0 <= value[1] < (1 << width),
                        'numeric pointer bit pattern')
                require(value[0] != 'null' or value[1] == 0, 'null pointer bit pattern')
                result = integer(value[1],dst)
            elif dst['kind']=='integer' and src['kind']=='integer' and op in (0,1,2,3,14):
                integer(value,src)
                if op==2: value%=1<<src['bits']
                elif op==3 and value & (1<<(src['bits']-1)): value=(value%(1<<src['bits']))-(1<<src['bits'])
                result=integer(value,dst)
            else: raise Unsupported('cast')
        elif kind=='binary': result=self.arithmetic(inst,values)
        elif kind=='compare':
            a,b=values; predicate=inst['predicate']; op=CMP.get(predicate)
            if predicate in (35,36):
                if not all(isinstance(v,tuple) and v[0] in ('function','null') for v in values):
                    numeric_null = (self.program['pointer_bits'] in (32, 64) and
                        any(v['kind'] == 'null' for v in operands) and
                        all(isinstance(v,tuple) and v[0] in ('absolute','null') and
                            type(v[1]) is int and 0 <= v[1] < (1 << self.program['pointer_bits']) and
                            ((v[0] == 'null') == (v[1] == 0)) for v in values))
                    heap_null = any(v['kind'] == 'null' for v in operands)
                    for operand, value in zip(operands, values):
                        if operand['kind'] == 'null':
                            heap_null &= value == ('null', 0)
                        else:
                            obj = (self.objects.get(value[0], {})
                                   if isinstance(value, tuple) and len(value) == 2 else {})
                            heap_null &= (isinstance(value, tuple) and len(value) == 2 and
                                          value[1] == 0 and obj.get('live') is True and
                                          'allocation' in obj and obj.get('owner') == 'main' and
                                          type(obj.get('occurrence')) is int and obj['occurrence'] >= 1)
                    if not (numeric_null or heap_null): raise Unsupported('pointer comparison')
                require((a==b) if predicate==35 else (a!=b), 'false pointer CFG guard')
            else:
                if op is None or not all(isinstance(v,int) for v in values): raise Unsupported('comparison')
                require({'eq':a==b,'ne':a!=b,'gt':a>b,'ge':a>=b,'lt':a<b,'le':a<=b}[op], 'false CFG guard')
        elif kind=='pointer_shift':
            result=self.pointer_shift(inst, values)
        elif kind in ('load','store'):
            if inst['atomic']: raise Unsupported('atomic memory operation')
            pointer=values[0]; t=inst['result']['type'] if kind=='load' else operands[1]['type']; width=int(t['size'])
            self.encoded_address(step,pointer,width)
            if kind=='load':
                result=self.load(pointer,t,inst['alignment'])
                if 'read_from' in step:
                    sources = [self.writers.get((pointer[0], pointer[1]+byte), 'initial') for byte in range(width)]
                    require(step['read_from'] == sources, 'SMT read source differs from actual last write')
            else: self.store(pointer,values[1],t,inst['alignment'])
            self.accesses[(trace_id,index)]=dict(instance=thread.instance,object=pointer[0],offset=pointer[1],width=width,
                                                write=kind=='store',vc=dict(thread.vc),atomic=self.atomic_owner==thread.instance)
        elif kind=='call':
            require(isinstance(values[0],tuple) and values[0][0]=='function', 'indirect callee')
            name=values[0][1]; fn=self.functions[name]
            if name == 'pthread_once' and fn['declaration']:
                self.once_call(values[1:], inst, step, trace_id, index, thread, frame)
                return
            if self.once_profile and any(f.once_pending is not None for f in thread.frames) and name in ('pthread_create', 'pthread_join'):
                raise Unsupported('once nested child lifecycle')
            if 'expanded_callee' in step: require(name == step['expanded_callee'], 'expanded callee differs from actual pointer')
            if not fn['declaration']:
                frame.pending=(inst,step); thread.frames.append(self.frame(name,values[1:]));
                if name.startswith('__VERIFIER_atomic_'):
                    require(self.atomic_owner is None, 'nested named atomic')
                    self.marker(trace_id, step['statement'], 'atomic_begin')
                return
            result=self.api(name,values[1:],inst,step,trace_id,index,thread,frame)
        elif kind=='return':
            result=values[0] if values else UNDEFINED
            if len(thread.frames) >= 2:
                parent = thread.frames[-2]
                pending = parent.once_pending
                if pending is not None and frame.function == pending['callback']:
                    require(not values and pending['role'] == 'active' and
                            step.get('once_callback_return') is True and
                            step.get('once_call_step') == pending['index'] and
                            pending.get('callback_frame') == frame.uid, 'once callback Return seal')
                    state = self.once_states[pending['control']]
                    require(state['phase'] == 'running' and state['owner'] == (thread.instance, frame.uid),
                            'once callback owner')
                    state.update(phase='done', completion=dict(thread.vc))
                    pending['returned'] = True
                    for obj, meta in self.objects.items():
                        if obj.startswith('stack:'+str(frame.uid)+':'): meta['live'] = False
                    thread.frames.pop()
                    self.marker(trace_id, pending['statement'], 'once_return', pending)
                    return
            require('once_callback_return' not in step, 'unexpected once callback Return')
            for obj, meta in self.objects.items():
                if obj.startswith('stack:'+str(frame.uid)+':'): meta['live']=False
            thread.frames.pop()
            if frame.function.startswith('__VERIFIER_atomic_'):
                require(thread.frames and thread.frames[-1].pending is not None, 'atomic invocation boundary')
                self.marker(trace_id, thread.frames[-1].pending[1]['statement'], 'atomic_end')
            if not thread.frames: thread.finished=True; return
            parent=thread.frames[-1]; call,call_step=parent.pending; parent.pending=None
            if 'result' in call and call['result']['type']['kind']=='integer':
                result=integer(result,call['result']['type'])
            self.assign(parent,call,call_step,result); return
        else: raise Unsupported('instruction '+kind)
        self.assign(frame,inst,step,result)

    def run(self):
        for trace in self.traces.values():
            if trace['entry'] != 'main':
                require(trace['creator_step'] < len(self.traces[trace['creator_trace']]['steps']),
                        'dynamic creation occurrence')
        roots=[k for k,t in self.traces.items() if t['entry']=='main']
        require(len(roots)==1, 'exactly one actual main prefix required')
        parameters = self.functions['main']['parameters']
        arguments = []
        if parameters:
            if len(parameters) != 2 or parameters[0]['type']['kind'] != 'integer' or parameters[1]['type']['kind'] != 'pointer':
                raise Unsupported('main entry signature')
            width = self.program['pointer_bits'] // 8
            string = 'environment:argv0'; argv = 'environment:argv'
            self.objects[string] = dict(size=1, live=True, variable=string, thread_local=False)
            self.memory[string] = [0]
            self.objects[argv] = dict(size=2*width, live=True, variable=argv, thread_local=False)
            self.memory[argv] = [('pointer-byte', (string,0), i) for i in range(width)] + [('pointer-byte', ('null',0), i) for i in range(width)]
            arguments = [1, (argv,0)]
        if 'ar.global_ctors' in self.program['globals']: raise Unsupported('global constructor execution')
        initial = self.frame('main',arguments)
        self.initial_main_frame = initial.uid
        self.threads[roots[0]]=Thread(0,[initial],{0:0})
        startup=self.traces.get('startup',{}).get('steps',[])
        for i,step in enumerate(startup): self.execute('startup',i,step)
        events=[]
        for name,trace in self.traces.items():
            if name=='startup': continue
            require(trace['steps'], 'empty selected prefix')
            times=[int(s['clock']) for s in trace['steps']]
            require(all(a<b for a,b in zip(times,times[1:])), 'program-order clocks')
            events.extend((int(s['clock']),name,i,s) for i,s in enumerate(trace['steps']))
        for _,name,i,step in sorted(events): self.execute(name,i,step)
        require(self.atomic_owner is None and not self.pending_markers, 'open synthetic/atomic prefix')
        require(all(f.once_pending is None for t in self.threads.values() for f in t.frames) and
                all(s['phase'] == 'done' for s in self.once_states.values()), 'open once execution')
        targets=self.certificate['targets']; require(len(targets)==2, 'target count')
        selected=[]
        for target in targets:
            key=(target['trace'],target['step'])
            require(key in self.accesses, 'target is not an actual memory occurrence')
            require(target['trace'] in ('a', 'b'), 'target trace identity')
            selected.append(self.accesses[key])
        a,b=selected
        require(not (a['atomic'] and b['atomic']), 'both targets in verifier atomics')
        # Match the producer's tail policy: neither target may omit later assumes.
        if a['atomic'] or b['atomic']:
            require(all(self.threads[t['trace']].finished for t in targets),
                    'atomic targets require completed root executions')
        require(a['instance']!=b['instance'], 'same dynamic target instance')
        require(a['object']==b['object'] and a['offset']<b['offset']+b['width'] and b['offset']<a['offset']+a['width'] and (a['write'] or b['write']), 'nonconflicting target ranges')
        before=lambda x,y: all(v<=y.get(k,0) for k,v in x.items())
        require(not before(a['vc'],b['vc']) and not before(b['vc'],a['vc']), 'targets ordered by pthread synchronization')
        # Serializing complete atomic sections is stronger than atomic communication.
        # Preserve the original pthread clocks: ordering only under this additional
        # relation prevents certification, without proving the execution invalid.
        def ordered_with_atomics(source, target):
            reachable = [source['vc']]
            for previous, current in zip(self.atomic_sections, self.atomic_sections[1:]):
                if any(before(vc, previous['end']) for vc in reachable):
                    reachable.append(current['begin'])
            return any(before(vc, target['vc']) for vc in reachable)
        if ordered_with_atomics(a, b) or ordered_with_atomics(b, a):
            raise Unsupported('targets ordered by conservative verifier-atomic synchronization')
        return dict(status='ACCEPT',scope='concrete_AR_prefix',exact_encoded_values=True,
                    instances=[a['instance'],b['instance']],targets=selected,steps=len(events)+len(startup),
                    scope_limit='AR/frontend and explicit library contracts; not source/witness validation')


def check(program, certificate):
    try: return Replay(program,certificate).run()
    except Reject as e: return dict(status='REJECT',reason=str(e))
    except Unsupported as e: return dict(status='UNSUPPORTED',reason=str(e))
    except (KeyError,TypeError,ValueError,IndexError) as e: return dict(status='REJECT',reason='malformed evidence: '+str(e))


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--program',type=Path,required=True)
    parser.add_argument('--certificate',type=Path,required=True)
    parser.add_argument('--input',type=Path,required=True)
    args=parser.parse_args()
    try:
        certificate=json.loads(args.certificate.read_text())
        require(certificate['input_sha256']==hashlib.sha256(args.input.read_bytes()).hexdigest(),'source hash mismatch')
        result=check(json.loads(args.program.read_text()),certificate)
    except (Reject, OSError, KeyError, ValueError) as error:
        result=dict(status='REJECT', reason=str(error))
    print(json.dumps(result,indent=2)); return 0 if result['status']=='ACCEPT' else 1


if __name__=='__main__':
    raise SystemExit(main())
