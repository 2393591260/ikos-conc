// Copyright and license: see data_race.hpp.
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/GetElementPtrTypeIterator.h>
#include <z3++.h>
#include <algorithm>
#include <deque>
#include <functional>
#include <ikos/analyzer/analysis/source_c_abi.hpp>
#include <ikos/analyzer/analysis/source_call_binding.hpp>
#include <ikos/analyzer/analysis/execution_engine/allocation_continuation.hpp>
#include <ikos/analyzer/checker/bucket_list_ownership.hpp>
#include <ikos/analyzer/checker/normal_mutex.hpp>
#include <ikos/analyzer/checker/data_race.hpp>
#include <ikos/analyzer/util/log.hpp>
#include <map>
#include <set>
#include <sstream>
#include <tuple>

namespace ikos {
namespace analyzer {
namespace {
struct Refuse {};
void need(bool p) {
  if (!p)
    throw Refuse{};
}
template < class M, class V >
void put(M& m, typename M::key_type k, const V& v) {
  auto i = m.find(k);
  if (i == m.end())
    m.emplace(k, v);
  else
    i->second = v;
}
bool scalar(llvm::Type* t) {
  return t->isIntegerTy() && t->getIntegerBitWidth() <= 64;
}
llvm::Type* value_type(llvm::Value* v) {
  auto* g = llvm::dyn_cast< llvm::GlobalVariable >(v);
  return g ? g->getValueType() : v->getType();
}
llvm::Function* target(llvm::CallBase* c) {
  return llvm::dyn_cast< llvm::Function >(
      c->getCalledOperand()->stripPointerCasts());
}
enum class Place { Null, Global, Local, Heap };
struct Pointer {
  Place place;
  std::uint64_t object;
  z3::expr offset;
  Pointer(z3::context& z, Place p = Place::Null, std::uint64_t id = 0)
      : place(p), object(id), offset(z.bv_val(0, 64)) {}
};
struct Key {
  std::uint64_t object, offset, width;
  bool operator<(const Key& b) const {
    return std::tie(object, offset, width) <
           std::tie(b.object, b.offset, b.width);
  }
};
struct Object {
  Place place;
  llvm::Value* source;
  std::uint64_t extent;
  bool born; // only existing normal continuation of real zero-base Store
  bool summary = false; // A set of nodes, never one concrete allocation identity.
};
struct State {
  std::map< llvm::Value*, z3::expr > values;
  std::map< llvm::Value*, Pointer > pointers;
  std::set< llvm::Value* > opaque;
  std::map< Key, z3::expr > scalars;
  std::map< Key, Pointer > links;
  std::map< std::uint64_t, Object > objects;
  std::set< std::uint64_t > initialized_mutexes, held;
  std::set< llvm::GlobalVariable* > auxiliary_held;
  std::map< std::uint64_t, std::uint64_t > owners;
  z3::expr path;
  bool spawned = false;
  explicit State(z3::context& z) : path(z.bool_val(true)) {}
};
struct Job {
  State state;
  llvm::Instruction* pc;
};
struct FunctionResult {
  State state;
  boost::optional< z3::expr > number;
  boost::optional< Pointer > pointer;
  bool opaque = false;
};
struct Argument {
  bool opaque = false;
  boost::optional< z3::expr > number;
  boost::optional< Pointer > pointer;
};
struct Endpoint {
  llvm::Instruction* statement;
  llvm::Function* root;
  std::vector< ar::CallBase* > calls;
  Place place;
  std::uint64_t object, offset, width;
  bool write, startup, payload, held;
  // For a private array, offset/width enclose every possible native-word access.
  bool private_range;
  boost::optional< std::uint64_t > owner;
  std::set< llvm::GlobalVariable* > auxiliary_held;
};
} // namespace
class BucketListOwnership::Impl {
  Context& ctx;
  const std::vector< AccessRecord >& accesses;
  z3::context z;
  llvm::Function* main = nullptr;
  unsigned address_bits = 0, word = 0, mutex_bytes = 0;
  llvm::GlobalVariable* global = nullptr;
  llvm::StructType* head_type = nullptr;
  llvm::StructType* node_type = nullptr;
  std::uint64_t global_id = 1, buckets = 0, head_begin = 0, head_stride = 0,
                mutex_begin = 0, mutex_stride = 0;
  std::uint64_t node_extent = 0, node_link = 0, datum_width = 0,
                prev_offset = 0, link_extent = 0;
  bool shared_graph = false;
  bool shared_payload_closed = false;
  // Sealed before any worker execution. Each load independently chooses a MAY
  // target; dropping correlations enlarges the set of possible executions.
  std::map< Key, std::vector< Pointer > > shared_edges;
  std::uint64_t tokens = 1;
  std::map< llvm::Value*, ar::Statement* > originals;
  std::map< llvm::Function*, ar::Function* > functions;
  std::set< llvm::Function* > active, workers, visited_functions;
  std::set< llvm::GlobalVariable* > literals, auxiliary_mutexes;
  std::set< llvm::CallBase* > auxiliary_locks;
  std::set< ar::Statement* > covered_native_calls;
  std::map< llvm::AllocaInst*, ar::LocalVariable* > native_arrays;

  std::vector< State > worlds;
  std::map< llvm::Instruction*, std::vector< Endpoint > > endpoints;
  std::set< llvm::Instruction* > original_mem_effects;
  std::vector< ar::CallBase* > context;
  llvm::Function* root = nullptr;
  unsigned budget = 500000, queries = 8192, symbols = 0;
  bool attempted = false, valid = false;
  llvm::Instruction* last = nullptr;
  void spend() {
    need(budget != 0);
    --budget;
  }
  z3::expr fresh(unsigned bits) {
    return z.bv_const(("bucket_" + std::to_string(++symbols)).c_str(), bits);
  }
  void set(State& s, llvm::Value* v, const z3::expr& x) {
    need(scalar(value_type(v)) &&
         x.get_sort().bv_size() == value_type(v)->getIntegerBitWidth());
    put(s.values, v, x.simplify());
    s.opaque.erase(v);
  }
  z3::expr number(const State& s, llvm::Value* v) {
    if (auto* n = llvm::dyn_cast< llvm::ConstantInt >(v)) {
      llvm::SmallString< 64 > text;
      n->getValue().toString(text, 10, false);
      return z.bv_val(text.c_str(), n->getBitWidth());
    }
    auto i = s.values.find(v);
    need(!s.opaque.count(v) && i != s.values.end());
    return i->second;
  }
  ar::Statement* original(llvm::Value* v) const {
    auto i = originals.find(v);
    return i == originals.end() ? nullptr : i->second;
  }
  ar::CallBase* binding(llvm::CallBase* c) const {
    auto* imported = source_call_binding(c, original(c));
    auto* called = target(c);
    if (!imported || !called ||
        !ordinary_source_call_abi_attributes(c, called))
      return nullptr;
    return imported;
  }
  ar::Value* pointer_copy_origin(ar::Value* value) const {
    // Importing an unsigned pthread_t cell or a callback can introduce a
    // representation-preserving pointer bitcast for an extern prototype.
    // Follow only unique pointer copies/bitcasts, never GEP/range equality.
    for (unsigned depth = 0; depth < 32; ++depth) {
      if (!value || !value->type()->is_pointer()) return nullptr;
      auto* variable = dyn_cast<ar::InternalVariable>(value);
      if (!variable) return value;
      auto* definition = unique_def(variable);
      if (auto* copy = dyn_cast_or_null<ar::Assignment>(definition))
        value = copy->operand();
      else if (auto* cast = dyn_cast_or_null<ar::UnaryOperation>(definition)) {
        if (cast->op() != ar::UnaryOperation::Bitcast) return nullptr;
        value = cast->operand();
      } else return value;
    }
    return nullptr;
  }
  bool nondet(llvm::CallBase* c) const {
    auto* a = binding(c);
    auto* f = target(c);
    auto* t = a ? native_handle_callee(a) : nullptr;
    if (!t || !f->isDeclaration() || !c->arg_empty() || !scalar(c->getType()))
      return false;
    const auto name = f->getName();
    const unsigned bits = c->getType()->getIntegerBitWidth();
    auto* result = a->result_or_null();
    auto* rt = result ? dyn_cast<ar::IntegerType>(result->type()) : nullptr;
    auto* ft = dyn_cast<ar::IntegerType>(t->type()->return_type());
    if (!f->getReturnType()->isIntegerTy(bits) || !rt || !ft ||
        rt->bit_width() != bits || ft->bit_width() != bits)
      return false;
    if ((name == "__VERIFIER_nondet_int" || name == "__VERIFIER_nondet_uint") &&
        bits == 32 && t->has_frontend())
      return true;
    if ((name == "__VERIFIER_nondet_ulonglong" ||
         name == "__VERIFIER_nondet_longlong") &&
        bits == 64 && t->has_frontend())
      return true;
    return c->getMetadata("ikos.freeze_uninit") &&
           t->intrinsic_id() == ar::Intrinsic::IkosNonDet &&
           ((name == "__ikos_nondet_int" && bits == 32) ||
            (name == "__ikos_nondet_int64" && bits == 64));
  }
  bool terminal(llvm::CallBase* c) const {
    auto* a = binding(c);
    auto* t = a ? native_handle_callee(a) : nullptr;
    auto* f = target(c);
    return t && f->isDeclaration() &&
           ((f->getName() == "abort" && c->arg_empty() &&
             c->getType()->isVoidTy() &&
             t->intrinsic_id() == ar::Intrinsic::LibcAbort) ||
            constant_assertion_effect(a));
  }

  z3::expr operation(State& s, llvm::Instruction* i) {
    if (auto* cast = llvm::dyn_cast< llvm::CastInst >(i)) {
      auto x = number(s, cast->getOperand(0));
      if (!scalar(cast->getType()))
        throw Refuse{};
      auto a = x.get_sort().bv_size(),
           b = cast->getType()->getIntegerBitWidth();
      switch (cast->getOpcode()) {
        case llvm::Instruction::SExt:
          return z3::sext(x, b - a);
        case llvm::Instruction::ZExt:
          return z3::zext(x, b - a);
        case llvm::Instruction::Trunc:
          return x.extract(b - 1, 0);
        case llvm::Instruction::BitCast:
          if (a == b)
            return x;
          break;
        default:
          break;
      }
      throw Refuse{};
    }
    if (auto* c = llvm::dyn_cast< llvm::ICmpInst >(i)) {
      auto a = number(s, c->getOperand(0)), b = number(s, c->getOperand(1));
      z3::expr p = z.bool_val(false);
      switch (c->getPredicate()) {
        case llvm::CmpInst::ICMP_EQ:
          p = a == b;
          break;
        case llvm::CmpInst::ICMP_NE:
          p = a != b;
          break;
        case llvm::CmpInst::ICMP_SLT:
          p = a < b;
          break;
        case llvm::CmpInst::ICMP_SLE:
          p = a <= b;
          break;
        case llvm::CmpInst::ICMP_SGT:
          p = a > b;
          break;
        case llvm::CmpInst::ICMP_SGE:
          p = a >= b;
          break;
        case llvm::CmpInst::ICMP_ULT:
          p = z3::ult(a, b);
          break;
        case llvm::CmpInst::ICMP_ULE:
          p = z3::ule(a, b);
          break;
        case llvm::CmpInst::ICMP_UGT:
          p = z3::ugt(a, b);
          break;
        case llvm::CmpInst::ICMP_UGE:
          p = z3::uge(a, b);
          break;
        default:
          throw Refuse{};
      }
      return z3::ite(p, z.bv_val(1, 1), z.bv_val(0, 1));
    }
    if (auto* b = llvm::dyn_cast< llvm::BinaryOperator >(i)) {
      auto a = number(s, b->getOperand(0)), c = number(s, b->getOperand(1));
      z3::expr r = a;
      switch (b->getOpcode()) {
        case llvm::Instruction::Add:
          r = a + c;
          break;
        case llvm::Instruction::Sub:
          r = a - c;
          break;
        case llvm::Instruction::Mul:
          r = a * c;
          break;
        case llvm::Instruction::And:
          return a & c;
        case llvm::Instruction::Or:
          return a | c;
        case llvm::Instruction::Xor:
          return a ^ c;
        default:
          throw Refuse{};
      }
      // Wrapping BV values overapproximate the defined executions of nsw/nuw
      // operations; poison is never used as an extra range assumption.
      return r;
    }
    if (auto* select = llvm::dyn_cast< llvm::SelectInst >(i))
      return z3::ite(number(s, select->getCondition()) == z.bv_val(1, 1),
                     number(s, select->getTrueValue()),
                     number(s, select->getFalseValue()));
    throw Refuse{};
  }
  bool feasible(const z3::expr& formula) {
    auto p = formula.simplify();
    if (p.is_true())
      return true;
    if (p.is_false())
      return false;
    need(queries != 0);
    --queries;
    z3::solver solver(z);
    solver.set("timeout", 100u);
    solver.add(p);
    auto result = solver.check();
    need(result != z3::unknown);
    return result == z3::sat;
  }
  void require(const State& s, const z3::expr& p) {
    need(!feasible(s.path && !p));
  }
  z3::expr widen(const z3::expr& x, bool sign = false) {
    need(x.get_sort().bv_size() <= 64);
    unsigned n = 64 - x.get_sort().bv_size();
    return sign ? z3::sext(x, n) : z3::zext(x, n);
  }
  std::uint64_t constant(const State& s, const z3::expr& value) {
    auto v = value.simplify();
    if (v.is_numeral())
      return v.get_numeral_uint64();
    need(queries != 0);
    --queries;
    z3::solver solver(z);
    solver.set("timeout", 100u);
    solver.add(s.path);
    auto r = solver.check();
    need(r == z3::sat);
    auto n = solver.get_model().eval(v, true);
    need(n.is_numeral());
    auto result = n.get_numeral_uint64();
    require(s, v == z.bv_val(result, v.get_sort().bv_size()));
    return result;
  }
  std::uint64_t bytes(llvm::Type* t) {
    need(t->isSized() && (!t->isPointerTy() ||
                         t->getPointerAddressSpace() == 0));
    const auto size = main->getParent()->getDataLayout().getTypeStoreSize(t);
    need(!size.isScalable() && size.getFixedValue() <= UINT32_MAX);
    return size.getFixedValue();
  }
  std::uint64_t extent(llvm::Type* t) {
    need(t->isSized());
    const auto size = main->getParent()->getDataLayout().getTypeAllocSize(t);
    // This theorem still covers bounded graph objects, not giant native heaps.
    need(!size.isScalable() && size.getFixedValue() <= UINT32_MAX);
    return size.getFixedValue();
  }
  Pointer pointer(const State& s, llvm::Value* v) {
    need(v && v->getType()->isPointerTy() &&
         v->getType()->getPointerAddressSpace() == 0);
    if (llvm::isa< llvm::ConstantPointerNull >(v))
      return Pointer(z);
    if (v == global)
      return Pointer(z, Place::Global, global_id);
    auto i = s.pointers.find(v);
    need(i != s.pointers.end());
    return i->second;
  }
  bool pointer_equal(const Pointer& a, const Pointer& b) const {
    return a.place == b.place && a.object == b.object &&
           z3::eq(a.offset, b.offset);
  }
  bool normal_pointer(const State& s, const Pointer& p) const {
    auto i = s.objects.find(p.object);
    return p.place != Place::Null && i != s.objects.end() && i->second.born;
  }
  void opaque(State& s, llvm::Value* v) {
    s.values.erase(v);
    s.opaque.insert(v);
  }
  // Undefined payload values are an explicit TOP. They can flow through scalar
  // arithmetic and scalar Stores, but using one in control/address/call
  // refuses.
  bool opaque_operation(State& s, llvm::Instruction* i) {
    bool top = false;
    for (auto& op : i->operands())
      top |= s.opaque.count(op.get());
    if (!top)
      return false;
    need(scalar(i->getType()) && (llvm::isa< llvm::BinaryOperator >(i) ||
                                  llvm::isa< llvm::CastInst >(i) ||
                                  llvm::isa< llvm::ICmpInst >(i)));
    opaque(s, i);
    return true;
  }
  bool pending_allocation_guard(const State& s, const Pointer& p,
                                llvm::ICmpInst* comparison) const {
    if (root != main || s.spawned || p.place != Place::Heap)
      return false;
    auto object = s.objects.find(p.object);
    if (object == s.objects.end() || object->second.born)
      return false;
    auto* call = llvm::dyn_cast< llvm::CallBase >(object->second.source);
    auto* allocation = call ? binding(call) : nullptr;
    if (!allocation)
      return false;
    ar::Comparison* guard = nullptr;
    return heap_initialization_successor(allocation->parent(), guard) &&
           guard && guard->has_frontend() &&
           guard->frontend< llvm::Value >() == comparison;
  }
  boost::optional< z3::expr > pointer_compare(const State& s, llvm::ICmpInst* c) {
    need(c->getPredicate() == llvm::CmpInst::ICMP_EQ ||
         c->getPredicate() == llvm::CmpInst::ICMP_NE);
    auto a = pointer(s, c->getOperand(0)), b = pointer(s, c->getOperand(1));
    // Before any spawn, a pure-abort failure edge can be omitted. The pending
    // object still needs the real Store contract before memory/graph use.
    for (const auto& p : {a, b}) {
      if (p.place == Place::Null) {
        // A nonzero AbsoluteZero address is not a disjoint NULL object.
        require(s, p.offset == z.bv_val(0, 64));
      } else {
        if (!normal_pointer(s, p)) {
          require(s, p.offset == z.bv_val(0, 64));
          need(pending_allocation_guard(s, p, c));
        }
        require(s,
                p.offset >= z.bv_val(0, 64) &&
                    z3::ult(p.offset,
                            z.bv_val(s.objects.at(p.object).extent, 64)));
      }
    }
    if (a.place == Place::Heap && b.place == Place::Heap &&
        (s.objects.at(a.object).summary || s.objects.at(b.object).summary))
      return {}; // Equal abstract tags cannot prove concrete pointer equality.
    z3::expr equal = z.bool_val(false);
    if (a.place == b.place && a.object == b.object)
      equal = a.offset == b.offset;
    if (c->getPredicate() == llvm::CmpInst::ICMP_NE)
      equal = !equal;
    return z3::ite(equal, z.bv_val(1, 1), z.bv_val(0, 1)).simplify();
  }
  Pointer gep(const State& s, llvm::GetElementPtrInst* g) {
    need(g->getAddressSpace() == 0);
    auto p = pointer(s, g->getPointerOperand());
    // Small graph offsets remain signed BV64, including temporary negative
    // container_of views. Each step is checked in BV128 before narrowing.
    // Native index<=64 bits times extent<=UINT32_MAX fits signed128; the
    // previous sum is bounded by UINT32_MAX, so even long GEPs cannot wrap.
    auto exact = z3::sext(p.offset, 64);
    const auto cap = z.bv_val(UINT64_C(0xffffffff), 128);
    require(s, exact >= -cap && exact <= cap);
    for (auto it = llvm::gep_type_begin(g), end = llvm::gep_type_end(g);
         it != end; ++it) {
      auto* index = it.getOperand();
      if (auto* st = it.getStructTypeOrNull()) {
        auto* n = llvm::dyn_cast<llvm::ConstantInt>(index);
        need(n && n->getValue().getActiveBits() <= 32 &&
             n->getZExtValue() < st->getNumElements());
        exact = exact + z.bv_val(main->getParent()->getDataLayout()
            .getStructLayout(st)->getElementOffset(n->getZExtValue()), 128);
      } else {
        need(scalar(index->getType()) &&
             index->getType()->getIntegerBitWidth() <= address_bits);
        auto value = number(s, index);
        exact = exact + z3::sext(value, 128 - value.get_sort().bv_size()) *
                          z.bv_val(extent(it.getIndexedType()), 128);
      }
      require(s, exact >= -cap && exact <= cap);
    }
    p.offset = exact.extract(63, 0).simplify();
    // No pointer-validity or live-allocation conclusion comes from this step.
    // resolve() must still prove every complete nonnegative byte interval.
    return p;
  }
  // A fully enumerated byte-offset alternative often fixes its source index.
  // Substitute only solver-proved singleton symbols; this preserves the whole
  // state and avoids re-proving the same index at every traversed graph edge.
  void specialize_offset(State& s, const z3::expr& offset) {
    if (!s.spawned || root == main)
      return;
    std::vector< z3::expr > pending{offset};
    std::set< unsigned > seen;
    z3::expr_vector from(z), to(z);
    while (!pending.empty()) {
      auto term = pending.back();
      pending.pop_back();
      if (!seen.insert(term.id()).second)
        continue;
      if (term.is_const() && term.is_bv() &&
          term.decl().decl_kind() == Z3_OP_UNINTERPRETED) {
        need(queries != 0);
        --queries;
        z3::solver solver(z);
        solver.set("timeout", 100u);
        solver.add(s.path);
        need(solver.check() == z3::sat);
        auto value = solver.get_model().eval(term, true);
        if (value.is_numeral() && !feasible(s.path && term != value)) {
          from.push_back(term);
          to.push_back(value);
        }
      } else if (term.is_app()) {
        for (unsigned n = 0; n < term.num_args(); ++n)
          pending.push_back(term.arg(n));
      }
    }
    if (from.empty())
      return;
    auto replace = [&](z3::expr value) {
      return value.substitute(from, to).simplify();
    };
    s.path = replace(s.path);
    for (auto& entry : s.values)
      entry.second = replace(entry.second);
    for (auto& entry : s.pointers)
      entry.second.offset = replace(entry.second.offset);
    for (auto& entry : s.scalars)
      entry.second = replace(entry.second);
    for (auto& entry : s.links)
      entry.second.offset = replace(entry.second.offset);
  }
  struct Resolution {
    State state;
    std::uint64_t offset;
  };
  std::vector< Resolution > resolve(State initial,
                                    const Pointer& p,
                                    std::uint64_t width) {
    need(p.place != Place::Null && width);
    auto oi = initial.objects.find(p.object);
    need(oi != initial.objects.end());
    const auto size = oi->second.extent;
    need(size <= UINT32_MAX && width <= size);
    require(initial,
            p.offset >= z.bv_val(0, 64) &&
                z3::ule(p.offset, z.bv_val(size - width, 64)));
    auto simplified = p.offset.simplify();
    if (simplified.is_numeral()) {
      std::vector< Resolution > one;
      one.push_back({std::move(initial), simplified.get_numeral_uint64()});
      return one;
    }
    std::vector< Resolution > result;
    auto remainder = initial.path;
    for (unsigned count = 0;; ++count) {
      need(count < 512 && queries != 0);
      --queries;
      z3::solver solver(z);
      solver.set("timeout", 100u);
      solver.add(remainder);
      auto r = solver.check();
      need(r != z3::unknown);
      if (r == z3::unsat)
        break;
      auto value = solver.get_model().eval(p.offset, true);
      need(value.is_numeral());
      auto n = value.get_numeral_uint64();
      State child = initial;
      child.path = (initial.path && (p.offset == value)).simplify();
      specialize_offset(child, p.offset);
      result.push_back({std::move(child), n});
      remainder = remainder && (p.offset != value);
    }
    need(!result.empty());
    return result;
  }
  bool overlap(const Key& a, const Key& b) const {
    return a.object == b.object && a.offset < b.offset + b.width &&
           b.offset < a.offset + a.width;
  }
  void invalidate(State& s, const Key& k) {
    for (auto it = s.scalars.begin(); it != s.scalars.end();)
      if (overlap(it->first, k))
        it = s.scalars.erase(it);
      else
        ++it;
    for (auto it = s.links.begin(); it != s.links.end();)
      if (overlap(it->first, k))
        it = s.links.erase(it);
      else
        ++it;
  }
  boost::optional< std::uint64_t > head_index(std::uint64_t offset) const {
    if (offset < head_begin || offset >= head_begin + buckets * head_stride)
      return {};
    return (offset - head_begin) / head_stride;
  }
  std::uint64_t mutex_index(std::uint64_t offset) const {
    need(offset >= mutex_begin &&
         offset < mutex_begin + buckets * mutex_stride &&
         (offset - mutex_begin) % mutex_stride == 0);
    return (offset - mutex_begin) / mutex_stride;
  }
  bool head_field(std::uint64_t off) const {
    auto index = head_index(off);
    if (!index)
      return false;
    auto n = off - head_begin - *index * head_stride;
    const auto link = shared_graph ? node_link : 0;
    return n == link || n == link + prev_offset;
  }
  bool global_payload(std::uint64_t off, std::uint64_t width,
                      llvm::Type* type) const {
    auto index = head_index(off);
    return shared_graph && index && width == datum_width && scalar(type) &&
           off == head_begin + *index * head_stride;
  }
  bool node_link_field(std::uint64_t off) const {
    return off == node_link || off == node_link + prev_offset;
  }
  void record(State& s,
              llvm::Instruction* i,
              const Pointer& p,
              std::uint64_t off,
              std::uint64_t width,
              bool write,
              bool payload,
              bool private_range = false) {
    Endpoint e{i,
               root,
               context,
               p.place,
               p.object,
               off,
               width,
               write,
               !s.spawned,
               payload,
               false,
               private_range,
               {},
               s.auxiliary_held};
    if (p.place == Place::Heap && s.spawned && !shared_graph) {
      auto o = s.owners.find(p.object);
      need(o != s.owners.end());
      e.owner = o->second;
      e.held = s.held.count(*e.owner);
      if (payload)
        need(e.held);
    }
    // Heap descriptors summarize all dynamic live objects with this owner;
    // they never claim one object per malloc site or one node per bucket.
    if (e.place == Place::Heap)
      e.object = 0;
    auto& out = endpoints[i];
    for (const auto& old : out)
      if (old.root == e.root && old.calls == e.calls && old.place == e.place &&
          old.object == e.object && old.offset == e.offset &&
          old.width == e.width && old.write == e.write &&
          old.startup == e.startup && old.payload == e.payload &&
          old.held == e.held && old.private_range == e.private_range && old.owner == e.owner &&
          old.auxiliary_held == e.auxiliary_held)
        return;
    out.push_back(std::move(e));
  }
  void memory(State& s,
              llvm::Instruction* i,
              const Pointer& p,
              std::uint64_t off,
              const Pointer* selected_link = nullptr) {
    auto* ld = llvm::dyn_cast< llvm::LoadInst >(i);
    auto* st = llvm::dyn_cast< llvm::StoreInst >(i);
    auto* a = original(i);
    need((ld && isa< ar::Load >(a) && !ld->isAtomic() && !ld->isVolatile()) ||
         (st && isa< ar::Store >(a) && !st->isAtomic() && !st->isVolatile()));
    auto* type = ld ? ld->getType() : st->getValueOperand()->getType();
    need(scalar(type) || type->isPointerTy());
    const auto width = bytes(type);
    Key key{p.object, off, width};
    auto& obj = s.objects.at(p.object);
    auto* ar_type = ld ? cast< ar::Load >(a)->result()->type()
                       : cast< ar::Store >(a)->value()->type();
    need(ctx.bundle->data_layout().store_size_in_bytes(ar_type) ==
         core::ZNumber(width));
    original_mem_effects.insert(i);
    bool payload = false;
    if (p.place == Place::Global) {
      payload = global_payload(off, width, type);
      need(p.object == global_id && (payload ||
           (type->isPointerTy() && width == word && head_field(off))));
      need(!st || !s.spawned || payload);
    } else if (p.place == Place::Heap) {
      need(obj.extent == node_extent);
      payload = off == 0 && width == datum_width && scalar(type);
      need(payload ||
           (type->isPointerTy() && width == word && node_link_field(off)));
      if (!obj.born) {
        // The existing imported Store performs prepare_mem_access on this exact
        // zero-base address. Failed malloc has no normal successor THERE, not
        // at allocation/GEP/helper-entry or an unknown-destination Store.
        need(st && !s.spawned && payload && off == 0);
        obj.born = true;
      }
      if (s.spawned)
        need(payload || !st);
    } else if (p.place == Place::Local) {
      need(off == 0 && width == obj.extent && scalar(type));
      // First scope scalar handles are never consumed after first create.
      need(!s.spawned);
    } else
      throw Refuse{};
    record(s, i, p, off, width, st, payload);
    if (type->isPointerTy()) {
      if (ld) {
        if (selected_link) put(s.pointers, i, *selected_link);
        else {
          auto old = s.links.find(key);
          need(old != s.links.end());
          put(s.pointers, i, old->second);
        }
      } else {
        auto value = pointer(s, st->getValueOperand());
        need(value.place == Place::Global || value.place == Place::Heap);
        need(normal_pointer(s, value));
        invalidate(s, key);
        put(s.links, key, value);
      }
    } else if (ld) {
      if (s.spawned && payload) {
        need(s.scalars.count(key));
        opaque(s, i);
      } else {
        auto old = s.scalars.find(key);
        need(old != s.scalars.end());
        set(s, i, old->second);
      }
    } else if (s.spawned) {
      // Scalar payload is havoced by arbitrary sibling steps; do not export a
      // store-dependent value into later control or reuse one chosen sample.
      need(payload && (p.place == Place::Heap || shared_graph));
      if (!s.opaque.count(st->getValueOperand()))
        (void)number(s, st->getValueOperand());
    } else {
      need(!s.opaque.count(st->getValueOperand()));
      auto v = number(s, st->getValueOperand());
      invalidate(s, key);
      put(s.scalars, key, v);
    }
  }
  void freeze(State& s) {
    need(!s.spawned && s.held.empty() && s.auxiliary_held.empty() &&
         s.initialized_mutexes.size() == buckets);
    if (shared_graph) {
      std::deque< Pointer > pending;
      for (std::uint64_t bucket = 0; bucket < buckets; ++bucket) {
        need(s.initialized_mutexes.count(bucket));
        Pointer head(z, Place::Global, global_id);
        head.offset = z.bv_val(head_begin + bucket * head_stride + node_link, 64);
        pending.push_back(head);
      }
      std::set< std::pair< std::uint64_t, std::uint64_t > > visited;
      std::set< std::uint64_t > reachable_heap;
      while (!pending.empty()) {
        spend();
        auto p = pending.front(); pending.pop_front();
        const auto off = constant(s, p.offset);
        need(normal_pointer(s, p));
        if (!visited.emplace(p.object, off).second) continue;
        if (p.place == Place::Global) {
          auto index = head_index(off);
          need(p.object == global_id && index &&
               off == head_begin + *index * head_stride + node_link &&
               s.scalars.count({global_id, off - node_link, datum_width}));
        } else {
          need(p.place == Place::Heap && off == node_link &&
               s.objects.at(p.object).extent == node_extent &&
               s.scalars.count({p.object, 0, datum_width}));
          reachable_heap.insert(p.object);
        }
        for (auto delta : {std::uint64_t(0), prev_offset}) {
          auto link = s.links.find({p.object, off + delta, word});
          need(link != s.links.end());
          pending.push_back(link->second);
        }
      }
      for (const auto& object : s.objects)
        if (object.second.place == Place::Heap)
          need(object.second.born && reachable_heap.count(object.first));
      s.spawned = true;
      return;
    }
    for (std::uint64_t bucket = 0; bucket < buckets; ++bucket) {
      need(s.initialized_mutexes.count(bucket));
      std::deque< Pointer > pending;
      Pointer h(z, Place::Global, global_id);
      h.offset = z.bv_val(head_begin + bucket * head_stride, 64);
      pending.push_back(h);
      std::set< std::pair< std::uint64_t, std::uint64_t > > visited;
      while (!pending.empty()) {
        spend();
        auto p = pending.front();
        pending.pop_front();
        auto off = constant(s, p.offset);
        need(normal_pointer(s, p));
        if (!visited.emplace(p.object, off).second)
          continue;
        if (p.place == Place::Global) {
          need(p.object == global_id &&
               off == head_begin + bucket * head_stride);
        } else {
          need(p.place == Place::Heap && off == node_link &&
               s.objects.at(p.object).extent == node_extent);
          auto old = s.owners.emplace(p.object, bucket);
          need(old.second || old.first->second == bucket);
          need(s.scalars.count({p.object, 0, datum_width}));
        }
        for (auto delta : {std::uint64_t(0), prev_offset}) {
          auto i = s.links.find({p.object, off + delta, word});
          need(i != s.links.end());
          pending.push_back(i->second);
        }
      }
    }
    for (const auto& kv : s.objects)
      if (kv.second.place == Place::Heap)
        need(kv.second.born && s.owners.count(kv.first));
    s.spawned = true;
  }
  // Simultaneous PHI evaluation is essential for loops with multiple carriers.
  llvm::Instruction* enter(State& s,
                           llvm::BasicBlock* block,
                           llvm::BasicBlock* predecessor) {
    std::vector< std::pair< llvm::PHINode*, Argument > > phis;
    std::set< llvm::PHINode* > tops;
    for (auto& i : *block) {
      auto* p = llvm::dyn_cast< llvm::PHINode >(&i);
      if (!p)
        break;
      need(predecessor);
      auto* v = p->getIncomingValueForBlock(predecessor);
      Argument a;
      if (v->getType()->isPointerTy())
        a.pointer = pointer(s, v);
      else if (s.opaque.count(v))
        tops.insert(p);
      else
        a.number = number(s, v);
      phis.emplace_back(p, std::move(a));
    }
    for (auto& kv : phis) {
      if (tops.count(kv.first))
        opaque(s, kv.first);
      else if (kv.second.pointer)
        put(s.pointers, kv.first, *kv.second.pointer);
      else
        set(s, kv.first, *kv.second.number);
    }
    return block->getFirstNonPHI();
  }
  std::string signature(const State& s, llvm::Instruction* pc) {
    std::ostringstream o;
    o << pc << '|' << s.path.simplify().to_string() << '|';
    for (const auto& v : s.values)
      o << v.first << ':' << v.second.to_string() << ';';
    o << '|';
    for (const auto& p : s.pointers)
      o << p.first << ':' << unsigned(p.second.place) << ':' << p.second.object
        << ':' << p.second.offset.to_string() << ';';
    o << '|';
    for (auto* v : s.opaque)
      o << v << ';';
    o << '|';
    for (auto v : s.held)
      o << v << ';';
    o << '|';
    for (auto* mutex : s.auxiliary_held)
      o << mutex << ';';
    // Worker links, objects, owners and initialized cells are immutable within
    // this execute invocation. Payload writes update TOP, never this snapshot.
    return o.str();
  }
  bool mutex_api(llvm::CallBase* c) const {
    auto* f = target(c);
    auto* a = binding(c);
    auto* t = a ? native_handle_callee(a) : nullptr;
    if (!f || !t || !f->isDeclaration() || !t->is_declaration() ||
        !c->getType()->isIntegerTy(32))
      return false;
    auto name = f->getName();
    bool init = name == "pthread_mutex_init";
    if (!init && name != "pthread_mutex_lock" && name != "pthread_mutex_unlock")
      return false;
    if ((init && t->name() != "pthread_mutex_init") ||
        (name == "pthread_mutex_lock" &&
         t->intrinsic_id() != ar::Intrinsic::PthreadMutexLock) ||
        (name == "pthread_mutex_unlock" &&
         t->intrinsic_id() != ar::Intrinsic::PthreadMutexUnlock) ||
        c->arg_size() != (init ? 2u : 1u) || !c->use_empty())
      return false;
    auto* pt =
        llvm::dyn_cast< llvm::PointerType >(c->getArgOperand(0)->getType());
    auto* st =
        pt ? llvm::dyn_cast< llvm::StructType >(pt->getPointerElementType())
           : nullptr;
    return pt && pt->getAddressSpace() == 0 && st && !st->isOpaque() &&
           st->isSized() && st->hasName() &&
           st->getName() == "union.pthread_mutex_t" &&
           !st->isPacked() &&
           main->getParent()->getDataLayout().getTypeAllocSize(st) == mutex_bytes &&
           main->getParent()->getDataLayout().getABITypeAlign(st).value() >= word &&
           (!init ||
            llvm::isa< llvm::ConstantPointerNull >(c->getArgOperand(1)));
  }
  bool literal_projection(llvm::Value* value) const {
    auto* source = llvm::dyn_cast< llvm::GlobalVariable >(value->stripPointerCasts());
    if (!source || !literals.count(source)) return false;
    std::vector< llvm::Value* > todo{value}; std::set< llvm::Value* > seen;
    while (!todo.empty()) {
      auto* v = todo.back(); todo.pop_back();
      if (!seen.insert(v).second) continue;
      if (seen.size() > 2048) return false;
      for (auto* user : v->users()) {
        if (auto* g = llvm::dyn_cast< llvm::GEPOperator >(user)) {
          if (g->getPointerOperand() != v || !g->hasAllZeroIndices()) return false;
          todo.push_back(user);
        } else if (llvm::isa< llvm::BitCastOperator >(user)) todo.push_back(user);
        else {
          auto* c = llvm::dyn_cast< llvm::CallBase >(user);
          auto* a = c ? binding(c) : nullptr;
          if (!a || !constant_assertion_effect(a)) return false;
        }
      }
    }
    return true;
  }
  bool branch_only(llvm::Value* value) const {
    std::vector< llvm::Value* > todo{value}; std::set< llvm::Value* > seen;
    while (!todo.empty()) {
      auto* v = todo.back(); todo.pop_back();
      if (!seen.insert(v).second) continue;
      if (seen.size() > 2048) return false;
      for (auto* user : v->users()) {
        if (auto* b = llvm::dyn_cast< llvm::BranchInst >(user)) {
          if (!b->isConditional() || b->getCondition() != v) return false;
          continue;
        }
        auto* i = llvm::dyn_cast< llvm::Instruction >(user);
        if (!i || !scalar(i->getType()) ||
            !(llvm::isa< llvm::BinaryOperator >(i) || llvm::isa< llvm::ICmpInst >(i) ||
              llvm::isa< llvm::CastInst >(i) || llvm::isa< llvm::PHINode >(i) ||
              llvm::isa< llvm::SelectInst >(i))) return false;
        todo.push_back(i);
      }
    }
    return true;
  }
  using Live = std::set< llvm::Value* >;
  std::map< llvm::Instruction*, Live > liveness(llvm::Function* f) {
    std::map< llvm::BasicBlock*, Live > input, output;
    auto local = [f](llvm::Value* v) {
      if (auto* i = llvm::dyn_cast< llvm::Instruction >(v)) return i->getFunction() == f;
      if (auto* a = llvm::dyn_cast< llvm::Argument >(v)) return a->getParent() == f;
      return false;
    };
    bool changed = true;
    while (changed) {
      changed = false;
      for (auto b = f->getBasicBlockList().rbegin(); b != f->getBasicBlockList().rend(); ++b) {
        spend(); Live out;
        for (auto* successor : llvm::successors(&*b)) {
          out.insert(input[successor].begin(), input[successor].end());
          for (auto& i : *successor) {
            auto* phi = llvm::dyn_cast< llvm::PHINode >(&i); if (!phi) break;
            auto* v = phi->getIncomingValueForBlock(&*b); if (local(v)) out.insert(v);
          }
        }
        Live in = out;
        for (auto i = b->rbegin(); i != b->rend(); ++i) {
          in.erase(&*i);
          if (!llvm::isa< llvm::PHINode >(*i))
            for (auto& operand : i->operands()) if (local(operand.get())) in.insert(operand.get());
        }
        if (in != input[&*b] || out != output[&*b]) { changed = true; input[&*b] = std::move(in); output[&*b] = std::move(out); }
      }
    }
    std::map< llvm::Instruction*, Live > result;
    for (auto& b : *f) {
      Live live = output[&b];
      for (auto i = b.rbegin(); i != b.rend(); ++i) {
        live.erase(&*i);
        if (!llvm::isa< llvm::PHINode >(*i))
          for (auto& operand : i->operands()) if (local(operand.get())) live.insert(operand.get());
        result.emplace(&*i, live);
      }
    }
    return result;
  }
  void project(State& s, llvm::Function* f, const Live& live) {
    auto dead = [&](llvm::Value* v) {
      auto* i = llvm::dyn_cast< llvm::Instruction >(v);
      auto* a = llvm::dyn_cast< llvm::Argument >(v);
      return ((i && i->getFunction() == f) || (a && a->getParent() == f)) && !live.count(v);
    };
    for (auto i = s.values.begin(); i != s.values.end();) if (dead(i->first)) i = s.values.erase(i); else ++i;
    for (auto i = s.pointers.begin(); i != s.pointers.end();) if (dead(i->first)) i = s.pointers.erase(i); else ++i;
    for (auto i = s.opaque.begin(); i != s.opaque.end();) if (dead(*i)) i = s.opaque.erase(i); else ++i;
    // Path constraints, memory, owners and caller frames are deliberately retained.
  }
  void native_suffix(llvm::Instruction* start, State& s) {
    need(ctx.native_handles_only && !ctx.native_handle_accesses.empty() && s.spawned &&
         root == main && context.empty() && !ctx.concurrent_env.is_dirty() &&
         ctx.concurrent_env.unmodeled_thread_creations().empty());
    for (const auto& access : ctx.native_handle_accesses) {
      // Final real-AI observations cover every call/load, iteration and context.
      // Native output/Load TOP and join's no-HB transfer do not depend on us.
      need(ctx.native_handle_ranges.proves(access.first));
    }
    auto access = [&](llvm::Instruction* i, bool write) {
      auto* a = original(i); auto found = ctx.native_handle_accesses.find(a);
      need(found != ctx.native_handle_accesses.end());
      llvm::AllocaInst* allocation = nullptr;
      for (const auto& array : native_arrays) if (array.second == found->second.storage) allocation = array.first;
      need(allocation && s.pointers.count(allocation));
      auto p = s.pointers.at(allocation); need(p.place == Place::Local && s.objects.at(p.object).extent >= word);
      // One enclosing range, used solely for object disjointness, not a slot identity.
      record(s, i, p, 0, s.objects.at(p.object).extent, write, false, true);
    };
    std::vector< llvm::Instruction* > todo{start}; std::set< llvm::Instruction* > seen;
    while (!todo.empty()) {
      auto* i = todo.back(); todo.pop_back(); spend();
      if (!seen.insert(i).second) continue;
      if (auto* c = llvm::dyn_cast< llvm::CallBase >(i)) {
        if (llvm::isa< llvm::DbgInfoIntrinsic >(c)) {}
        else {
          auto* a = binding(c); need(a);
          if (terminal(c)) continue;
          if (native_handle_api(c, true)) {
            need(ctx.native_handle_accesses.count(a));
            auto* worker = llvm::cast< llvm::Function >(c->getArgOperand(2)->stripPointerCasts());
            need(functions.count(worker)); workers.insert(worker); access(i, true);
          } else need(native_handle_api(c, false) && ctx.native_handle_joins.count(a));
          covered_native_calls.insert(a);
        }
      } else if (auto* load = llvm::dyn_cast< llvm::LoadInst >(i)) {
        need(load->getType()->isIntegerTy(address_bits) && !load->isAtomic() && !load->isVolatile()); access(i, false);
      } else if (auto* gep = llvm::dyn_cast< llvm::GetElementPtrInst >(i)) {
        auto* allocation = llvm::dyn_cast< llvm::AllocaInst >(gep->getPointerOperand()->stripPointerCasts());
        need(allocation && native_arrays.count(allocation) && native_handle_element(gep, allocation));
      } else if (auto* ret = llvm::dyn_cast< llvm::ReturnInst >(i)) {
        need(ret->getReturnValue() && ret->getReturnValue()->getType()->isIntegerTy(32)); continue;
      } else if (llvm::isa< llvm::BranchInst >(i)) {
        for (auto* successor : llvm::successors(i->getParent())) todo.push_back(&successor->front());
        continue;
      } else need(scalar(i->getType()) &&
                  (llvm::isa< llvm::PHINode >(i) || llvm::isa< llvm::BinaryOperator >(i) ||
                   llvm::isa< llvm::ICmpInst >(i) || llvm::isa< llvm::SelectInst >(i) ||
                   llvm::isa< llvm::CastInst >(i)));
      need(i->getNextNode()); todo.push_back(i->getNextNode());
    }
  }
  std::vector< FunctionResult > execute(
      llvm::Function* f,
      State initial,
      const std::vector< Argument >& arguments) {
    need(functions.count(f) && active.insert(f).second && active.size() <= 16 &&
         !f->isVarArg() && f->getCallingConv() == llvm::CallingConv::C &&
         ordinary_source_function_abi_attributes(f) &&
         f->arg_size() == arguments.size());
    visited_functions.insert(f);
    for (unsigned n = 0; n < f->arg_size(); ++n) {
      initial.values.erase(f->getArg(n));
      initial.pointers.erase(f->getArg(n));
      initial.opaque.erase(f->getArg(n));
      if (arguments[n].opaque)
        opaque(initial, f->getArg(n));
      else if (arguments[n].pointer)
        put(initial.pointers, f->getArg(n), *arguments[n].pointer);
      else {
        need(bool(arguments[n].number));
        set(initial, f->getArg(n), *arguments[n].number);
      }
    }
    // Discard stale callee SSA values from earlier dynamic invocations. Memory
    // cells/tokens are retained; dynamic allocation identity never uses an SSA.
    for (auto& b : *f)
      for (auto& i : b) {
        initial.values.erase(&i);
        initial.pointers.erase(&i);
        initial.opaque.erase(&i);
      }
    llvm::DominatorTree dom(*f);
    llvm::LoopInfo loops(dom);
    std::set< llvm::Instruction* > headers;
    std::function< void(llvm::Loop*) > add_loop = [&](llvm::Loop* l) {
      headers.insert(l->getHeader()->getFirstNonPHI());
      for (auto* c : l->getSubLoops())
        add_loop(c);
    };
    for (auto* l : loops)
      add_loop(l);
    auto live = liveness(f);
    for (auto& b : *f) if (llvm::pred_size(&b) > 1) headers.insert(b.getFirstNonPHI());
    std::set< std::string > visited_states;
    std::deque< Job > pending;
    pending.push_back(
        {std::move(initial), f->getEntryBlock().getFirstNonPHI()});
    std::vector< FunctionResult > results;
    while (!pending.empty()) {
      Job job = std::move(pending.front());
      pending.pop_front();
      auto& s = job.state;
      while (job.pc) {
        spend();
        auto* i = job.pc;
        last = i;
        if (s.spawned && root != main && headers.count(i)) {
          project(s, f, live.at(i));
          if (!visited_states.insert(signature(s, i)).second) break;
        }
        if (auto* br = llvm::dyn_cast< llvm::BranchInst >(i)) {
          if (!br->isConditional()) {
            job.pc = enter(s, br->getSuccessor(0), i->getParent());
            continue;
          }
          if (s.opaque.count(br->getCondition())) {
            for (unsigned n = 0; n < 2; ++n) {
              State child = s;
              auto* next = enter(child, br->getSuccessor(n), i->getParent());
              pending.push_back({std::move(child), next});
            }
            break;
          }
          auto condition = number(s, br->getCondition()).simplify();
          if (condition.is_numeral()) {
            job.pc =
                enter(s,
                      br->getSuccessor(condition.get_numeral_uint64() ? 0 : 1),
                      i->getParent());
            continue;
          }
          for (unsigned n = 0; n < br->getNumSuccessors(); ++n) {
            State child = s;
            if (br->isConditional())
              child.path = (s.path && (number(s, br->getCondition()) ==
                                       z.bv_val(n == 0 ? 1 : 0, 1)))
                               .simplify();
            if (!feasible(child.path))
              continue;
            auto* next = enter(child, br->getSuccessor(n), i->getParent());
            pending.push_back({std::move(child), next});
          }
          break;
        }
        if (auto* ret = llvm::dyn_cast< llvm::ReturnInst >(i)) {
          need(s.held.empty() || root == main || f != root);
          need(s.auxiliary_held.empty() || f != root);
          FunctionResult r{std::move(s), {}, {}, false};
          if (auto* v = ret->getReturnValue()) {
            if (v->getType()->isPointerTy()) {
              r.pointer = pointer(r.state, v);
              if (r.pointer->place != Place::Null)
                need(normal_pointer(r.state, *r.pointer));
            } else if (r.state.opaque.count(v))
              r.opaque = true;
            else
              r.number = number(r.state, v);
          }
          // Capture the return first, then discard only this completed frame's SSA.
          for (auto& arg : f->args()) { r.state.values.erase(&arg); r.state.pointers.erase(&arg); r.state.opaque.erase(&arg); }
          for (auto& block : *f) for (auto& instruction : block) {
            r.state.values.erase(&instruction); r.state.pointers.erase(&instruction); r.state.opaque.erase(&instruction);
          }
          results.push_back(std::move(r));
          break;
        }
        if (llvm::isa< llvm::UnreachableInst >(i))
          throw Refuse{};
        if (auto* alloca = llvm::dyn_cast< llvm::AllocaInst >(i)) {
          auto* count =
              llvm::dyn_cast< llvm::ConstantInt >(alloca->getArraySize());
          auto* a = dyn_cast_or_null< ar::Allocate >(original(i));
          need(root == main && !s.spawned && a && count && count->isOne() &&
               alloca->getAddressSpace() == 0 &&
               (scalar(alloca->getAllocatedType()) || native_arrays.count(alloca)) &&
               alloca->getParent() == &f->getEntryBlock());
          auto id = ++tokens;
          s.objects.emplace(id,
                            Object{Place::Local,
                                   alloca,
                                   extent(alloca->getAllocatedType()),
                                   true});
          put(s.pointers, i, Pointer(z, Place::Local, id));
        } else if (auto* g = llvm::dyn_cast< llvm::GetElementPtrInst >(i)) {
          if (!literal_projection(g)) put(s.pointers, i, gep(s, g));
        }
        else if (auto* cast = llvm::dyn_cast< llvm::BitCastInst >(i)) {
          if (llvm::isa< llvm::Function >(
                  cast->getOperand(0)->stripPointerCasts())) {
            auto* function = llvm::cast< llvm::Function >(
                cast->getOperand(0)->stripPointerCasts());
            for (auto* user : cast->users()) {
              auto* c = llvm::dyn_cast< llvm::CallBase >(user);
              need(c && c->getCalledOperand() == cast && target(c) == function);
            }
          } else if (auxiliary_mutexes.count(llvm::dyn_cast< llvm::GlobalVariable >(cast->getOperand(0)->stripPointerCasts()))) {
            // Its entire use closure is restricted to exact normal lock/unlock calls.
          } else if (cast->getType()->isPointerTy() &&
                     cast->getOperand(0)->getType()->isPointerTy()) {
            need(cast->getType()->getPointerAddressSpace() == 0 &&
                 cast->getOperand(0)->getType()->getPointerAddressSpace() == 0);
            put(s.pointers, i, pointer(s, cast->getOperand(0)));
          } else if (!opaque_operation(s, i))
            set(s, i, operation(s, i));
        } else if (auto* projection = llvm::dyn_cast< llvm::PtrToIntInst >(i)) {
          auto p = pointer(s, projection->getOperand(0));
          if (p.place == Place::Null) {
            need(projection->getType()->isIntegerTy(address_bits));
            auto n = p.offset.simplify();
            need(n.is_numeral() && n.get_numeral_uint64() <= UINT32_MAX);
            set(s, i, n.extract(address_bits - 1, 0));
          } else {
            // Program exit code is not an application-memory observation.
            // Never turn this address into an integer usable by the program.
            need(root == main && main_exit_pointer_projection(projection));
            opaque(s, i);
          }
        } else if (auto* cmp = llvm::dyn_cast< llvm::ICmpInst >(i)) {
          if (!opaque_operation(s, i)) {
            if (cmp->getOperand(0)->getType()->isPointerTy()) {
              auto result = pointer_compare(s, cmp);
              if (result) set(s, i, *result);
              else opaque(s, i);
            } else set(s, i, operation(s, i));
          }
        } else if (llvm::isa< llvm::LoadInst >(i) ||
                   llvm::isa< llvm::StoreInst >(i)) {
          auto* ld = llvm::dyn_cast< llvm::LoadInst >(i);
          auto* st = llvm::dyn_cast< llvm::StoreInst >(i);
          auto p =
              pointer(s,
                      ld ? ld->getPointerOperand() : st->getPointerOperand());
          auto* type = ld ? ld->getType() : st->getValueOperand()->getType();
          auto alternatives = resolve(std::move(s), p, bytes(type));
          for (auto& resolution : alternatives) {
            if (shared_graph && root != main && ld && type->isPointerTy()) {
              auto targets = shared_edges.find({p.object, resolution.offset, bytes(type)});
              need(targets != shared_edges.end() && !targets->second.empty());
              for (const auto& target : targets->second) {
                State child = resolution.state;
                memory(child, i, p, resolution.offset, &target);
                pending.push_back({std::move(child), i->getNextNode()});
              }
            } else {
              memory(resolution.state, i, p, resolution.offset);
              pending.push_back({std::move(resolution.state), i->getNextNode()});
            }
          }
          break;
        } else if (auto* call = llvm::dyn_cast< llvm::CallBase >(i)) {
          auto* a = binding(call);
          auto* called = target(call);
          need(a && called);
          if (terminal(call))
            break;
          if (nondet(call)) {
            if (call->use_empty() || (root != main && branch_only(call))) opaque(s, i);
            else set(s, i, fresh(call->getType()->getIntegerBitWidth()));
          }
          else if (!called->isDeclaration()) {
            std::vector< Argument > args;
            for (auto& arg : call->args()) {
              Argument v;
              if (arg->getType()->isPointerTy())
                v.pointer = pointer(s, arg.get());
              else if (s.opaque.count(arg.get()))
                v.opaque = true;
              else
                v.number = number(s, arg.get());
              args.push_back(std::move(v));
            }
            context.push_back(a);
            auto children = execute(called, std::move(s), args);
            context.pop_back();
            for (auto& result : children) {
              if (call->getType()->isPointerTy()) {
                need(bool(result.pointer));
                put(result.state.pointers, i, *result.pointer);
              } else if (!call->getType()->isVoidTy()) {
                if (result.opaque)
                  opaque(result.state, i);
                else {
                  need(bool(result.number));
                  set(result.state, i, *result.number);
                }
              }
              pending.push_back({std::move(result.state), i->getNextNode()});
            }
            break;
          } else if (native_handle_callee(a)->intrinsic_id() ==
                     ar::Intrinsic::LibcMalloc) {
            need(root == main && !s.spawned && called->getName() == "malloc" &&
                 call->arg_size() == 1 && call->getType()->isPointerTy() &&
                 call->getType()->getPointerAddressSpace() == 0 &&
                 call->getType()->getPointerElementType()->isIntegerTy(8) &&
                 call->getArgOperand(0)->getType()->isIntegerTy(address_bits) &&
                 source_integer_binding(a->argument(0), call->getArgOperand(0)));
            auto size = constant(s, number(s, call->getArgOperand(0)));
            need(size == node_extent);
            auto id = ++tokens;
            s.objects.emplace(id, Object{Place::Heap, call, size, false});
            put(s.pointers, i, Pointer(z, Place::Heap, id));
          } else if (auxiliary_locks.count(call)) {
            if (shared_graph) {
              const auto mutex = normal_mutex(ctx, a);
              need(mutex.source && auxiliary_mutexes.count(mutex.source));
              if (mutex.lock)
                need(s.auxiliary_held.insert(mutex.source).second);
              else
                need(s.auxiliary_held.erase(mutex.source) == 1);
            }
            opaque(s, i); // No numerical result or bucket-owner fact.
          } else if (mutex_api(call)) {
            auto p = pointer(s, call->getArgOperand(0));
            need(p.place == Place::Global && p.object == global_id);
            auto alternatives = resolve(std::move(s), p, mutex_stride);
            for (auto& resolution : alternatives) {
              auto& child = resolution.state;
              auto index = mutex_index(resolution.offset);
              auto name = called->getName();
              if (name == "pthread_mutex_init")
                need(root == main && !child.spawned &&
                     child.initialized_mutexes.insert(index).second);
              else {
                need(root != main && child.spawned &&
                     child.initialized_mutexes.count(index));
                if (name == "pthread_mutex_lock")
                  need(child.held.insert(index).second);
                else
                  need(child.held.erase(index) == 1);
              }
              // Valid default-normal lifecycle: the established native contract
              // supplies ownership on normal lock return, never from a MAYset.
              set(child, i, z.bv_val(0, 32));
              pending.push_back({std::move(child), i->getNextNode()});
            }
            break;
          } else if (native_handle_api(call, true)) {
            need(
                root == main && f == main && context.empty() &&
                llvm::isa< llvm::ConstantPointerNull >(
                    call->getArgOperand(1)) &&
                llvm::isa< llvm::ConstantPointerNull >(call->getArgOperand(3)));
            auto* worker = llvm::dyn_cast< llvm::Function >(
                call->getArgOperand(2)->stripPointerCasts());
            need(worker && worker != main && !worker->isDeclaration() &&
                 worker->arg_size() == 1 &&
                 worker->getArg(0)->getType()->isPointerTy() &&
                 worker->getReturnType()->isPointerTy());
            workers.insert(worker);
            auto p = pointer(s, call->getArgOperand(0));
            need(p.place == Place::Local);
            auto* allocation = llvm::dyn_cast< llvm::AllocaInst >(
                s.objects.at(p.object).source);
            if (allocation && native_arrays.count(allocation)) {
              if (!s.spawned) { freeze(s); worlds.push_back(s); }
              native_suffix(call, s);
              // The suffix may return or diverge; neither result supplies completion.
              results.push_back(FunctionResult{std::move(s), {}, {}, false});
              break;
            }
            auto* imported = allocation
                ? dyn_cast_or_null<ar::Allocate>(original(allocation)) : nullptr;
            auto* repetitions = imported
                ? dyn_cast<ar::IntegerConstant>(imported->array_size()) : nullptr;
            auto* cell = imported
                ? dyn_cast<ar::IntegerType>(imported->allocated_type()) : nullptr;
            auto* child = dyn_cast_or_null<ar::FunctionPointerConstant>(pointer_copy_origin(a->argument(2)));
            need(constant(s, p.offset) == 0 &&
                 s.objects.at(p.object).extent == word && allocation &&
                 allocation->getAllocatedType()->isIntegerTy(address_bits) &&
                 allocation->getAlign().value() >= word &&
                 imported && unique_allocation_definition(imported) &&
                 repetitions && repetitions->value().to_z_number() == 1 &&
                 cell && cell->bit_width() == address_bits &&
                 imported->result()->alignment() >= word &&
                 ctx.bundle->data_layout().alloc_size_in_bytes(cell) == word &&
                 pointer_copy_origin(a->argument(0)) == imported->result() &&
                 isa<ar::NullConstant>(a->argument(1)) &&
                 isa<ar::NullConstant>(a->argument(3)) && child &&
                 child->function()->has_frontend() &&
                 child->function()->frontend<llvm::Value>() == worker);
            if (!s.spawned) {
              freeze(s);
              worlds.push_back(s);
            }
            record(s, i, p, 0, word, true, false);
            // Native output is unconstrained; first scope forbids consuming it.
            invalidate(s, {p.object, 0, word});
            set(s, i, fresh(32));
          } else
            throw Refuse{};
        } else if (scalar(i->getType())) {
          if (!opaque_operation(s, i))
            set(s, i, operation(s, i));
        } else
          throw Refuse{};
        job.pc = i->getNextNode();
      }
    }
    active.erase(f);
    return results;
  }
  void inventory() {
    need(ctx.opts.enable_thread_modular &&
         ctx.opts.procedural != Procedural::Intraprocedural &&
         ctx.opts.entry_points.size() == 1 &&
         ctx.opts.entry_points.front()->name() == "main" &&
         ctx.opts.entry_points.front()->has_frontend() &&
         ctx.opts.no_init_globals.empty() &&
         !ctx.bundle->global_or_null("ar.global_ctors") &&
         !ctx.bundle->global_or_null("ar.global_dtors"));
    main = ctx.opts.entry_points.front()->frontend< llvm::Function >();
    auto* module = main->getParent();
    const auto abi = pthread_object_layout(ctx.bundle, module);
    need(bool(abi) &&
         module->getDataLayout().getIndexSizeInBits(0) == abi.word * 8);
    word = abi.word;
    mutex_bytes = abi.mutex;
    address_bits = word * 8;
    need(main->use_empty() && main->arg_empty() &&
         main->getReturnType()->isIntegerTy(32) &&
         module->getModuleInlineAsm().empty() &&
         !module->getNamedGlobal("llvm.global_ctors") &&
         !module->getNamedGlobal("llvm.global_dtors"));
    // Clang can lower a zero mutex initializer to an anonymous literal type.
    // Classify it through its actual API and AR binding, never size alone.
    std::set< llvm::GlobalVariable* > mutex_candidates;
    for (auto it = ctx.bundle->function_begin();
         it != ctx.bundle->function_end(); ++it) {
      if (!(*it)->is_definition()) continue;
      for (auto* block : *(*it)->body()) for (auto* statement : *block) {
        spend();
        auto* call = dyn_cast< ar::CallBase >(statement);
        auto* source = call && call->has_frontend()
            ? llvm::dyn_cast< llvm::CallInst >(call->frontend< llvm::Value >())
            : nullptr;
        if (source && source_call_binding(source, call) == call) {
          const auto mutex = normal_mutex(ctx, call);
          if (mutex.source) mutex_candidates.insert(mutex.source);
        }
      }
    }
    for (auto& g : module->globals()) {
      if (g.isConstant()) {
        auto* text = g.hasInitializer() ? llvm::dyn_cast< llvm::ConstantDataArray >(g.getInitializer()) : nullptr;
        need(text && text->isCString() && g.getAddressSpace() == 0 && !g.isThreadLocal() && !g.isExternallyInitialized());
        literals.insert(&g); continue;
      }
      auto* type = llvm::dyn_cast< llvm::StructType >(g.getValueType());
      if (type && !type->isOpaque() && !type->isPacked() &&
          mutex_candidates.count(&g) && extent(type) == mutex_bytes) {
        auxiliary_mutexes.insert(&g); continue;
      }
      need(!global && !g.isThreadLocal() && !g.isExternallyInitialized() &&
           g.getAddressSpace() == 0 && g.hasInitializer() && g.getInitializer()->isNullValue());
      global = &g;
    }
    need(global);
    auto* gtype = llvm::dyn_cast< llvm::StructType >(global->getValueType());
    need(gtype && !gtype->isOpaque() && !gtype->isPacked() && gtype->getNumElements() == 2);
    auto* heads = llvm::dyn_cast< llvm::ArrayType >(gtype->getElementType(0));
    auto* mutexes = llvm::dyn_cast< llvm::ArrayType >(gtype->getElementType(1));
    head_type =
        heads ? llvm::dyn_cast< llvm::StructType >(heads->getElementType())
              : nullptr;
    auto* global_node = head_type;
    if (head_type && !head_type->isOpaque() &&
        head_type->getNumElements() == 2 &&
        scalar(head_type->getElementType(0))) {
      shared_graph = true;
      head_type = llvm::dyn_cast< llvm::StructType >(head_type->getElementType(1));
    }
    auto* mtype =
        mutexes ? llvm::dyn_cast< llvm::StructType >(mutexes->getElementType())
                : nullptr;
    need(heads && mutexes && head_type && !head_type->isOpaque() &&
         !head_type->isPacked() && head_type->getNumElements() == 2 &&
         mtype && !mtype->isOpaque() && !mtype->isPacked() &&
         mtype->hasName() && mtype->getName() == "union.pthread_mutex_t" &&
         extent(mtype) == mutex_bytes && heads->getNumElements() &&
         heads->getNumElements() <= 64 &&
         heads->getNumElements() == mutexes->getNumElements());
    for (auto* type : head_type->elements()) {
      auto* p = llvm::dyn_cast< llvm::PointerType >(type);
      need(p && p->getAddressSpace() == 0 &&
           p->getPointerElementType() == head_type);
    }
    const auto& dl = module->getDataLayout();
    auto* hl = dl.getStructLayout(head_type);
    auto* gl = dl.getStructLayout(gtype);
    buckets = heads->getNumElements();
    link_extent = extent(head_type);
    head_stride = extent(heads->getElementType());
    prev_offset = hl->getElementOffset(1);
    head_begin = gl->getElementOffset(0);
    mutex_begin = gl->getElementOffset(1);
    mutex_stride = extent(mtype);
    need(hl->getElementOffset(0) == 0 && prev_offset == word &&
         link_extent == 2 * word &&
         dl.getABITypeAlign(head_type).value() >= word &&
         dl.getABITypeAlign(mtype).value() >= word &&
         head_begin % word == 0 && head_stride % word == 0 &&
         mutex_begin % word == 0 && mutex_stride % word == 0 &&
         head_begin + buckets * head_stride <= mutex_begin &&
         mutex_begin + buckets * mutex_stride <= extent(gtype) &&
         extent(gtype) <= UINT32_MAX &&
         (global->getAlign() ? global->getAlign()->value()
                             : dl.getABITypeAlign(gtype).value()) >= word);
    for (auto* type : module->getIdentifiedStructTypes()) {
      if (type->isOpaque() || type->getNumElements() != 2 ||
          !scalar(type->getElementType(0)) ||
          type->getElementType(1) != head_type)
        continue;
      need(!node_type);
      need(!shared_graph || type == global_node);
      node_type = type;
      auto* layout = dl.getStructLayout(type);
      need(!type->isPacked() && layout->getElementOffset(0) == 0 &&
           dl.getABITypeAlign(type).value() >= word);
      node_link = layout->getElementOffset(1);
      datum_width = bytes(type->getElementType(0));
      node_extent = extent(type);
      need(datum_width && datum_width <= 8 && datum_width <= node_link &&
           node_link % word == 0 && node_extent % word == 0 &&
           node_link + link_extent <= node_extent && node_extent <= UINT32_MAX);
    }
    need(node_type);
    need(!shared_graph || head_stride == node_extent);
    for (auto it = ctx.bundle->function_begin();
         it != ctx.bundle->function_end();
         ++it) {
      if (!(*it)->is_definition())
        continue;
      need((*it)->has_frontend());
      auto* f = (*it)->frontend< llvm::Function >();
      need(functions.emplace(f, *it).second);
      for (auto* b : *(*it)->body())
        for (auto* a : *b) {
          spend();
          if (!isa< ar::CallBase >(a) && !isa< ar::Load >(a) &&
              !isa< ar::Store >(a) && !isa< ar::Allocate >(a))
            continue;
          auto* i = a->has_frontend() ? llvm::dyn_cast< llvm::Instruction >(
                                            a->frontend< llvm::Value >())
                                      : nullptr;
          need(i && i->getFunction() == f && originals.emplace(i, a).second);
        }
    }
    for (const auto& original : originals) {
      auto* allocation = dyn_cast< ar::Allocate >(original.second);
      auto* source = llvm::dyn_cast< llvm::AllocaInst >(original.first);
      if (!allocation || !source || source->getFunction() != main) continue;
      for (const auto& access : ctx.native_handle_accesses)
        if (access.second.storage == allocation->result()) native_arrays.emplace(source, allocation->result());
    }
    // Verify imported global initialization agrees with the exact LLVM object.
    unsigned found = 0;
    for (auto it = ctx.bundle->global_begin(); it != ctx.bundle->global_end();
         ++it) {
      auto* g = *it;
      need(g->has_frontend() && g->is_definition() && !g->is_thread_local());
      if (g->frontend< llvm::Value >() != global) {
        auto* source = llvm::dyn_cast< llvm::GlobalVariable >(g->frontend< llvm::Value >());
        need(source && (literals.count(source) || auxiliary_mutexes.count(source)));
        continue;
      }
      need(ctx.bundle->data_layout().alloc_size_in_bytes(
               g->type()->pointee()) == core::ZNumber(extent(gtype)));
      for (auto* b : *g->initializer())
        for (auto* a : *b) {
          auto* st = dyn_cast< ar::Store >(a);
          need(st && st->pointer() == g &&
               isa< ar::AggregateZeroConstant >(st->value()) &&
               st->value()->type() == g->type()->pointee());
          ++found;
        }
    }
    need(found == 1);
    for (auto* g : literals) need(literal_projection(g));
    for (auto* g : auxiliary_mutexes) {
      std::vector< llvm::Value* > pending{g}; std::set< llvm::Value* > seen;
      bool used = false;
      while (!pending.empty()) {
        auto* v = pending.back(); pending.pop_back(); spend();
        if (!seen.insert(v).second) continue;
        for (auto* user : v->users()) {
          if (llvm::isa< llvm::BitCastOperator >(user)) { pending.push_back(user); continue; }
          auto* c = llvm::dyn_cast< llvm::CallBase >(user);
          auto m = c ? normal_mutex(ctx, binding(c)) : NormalMutex{};
          need(c && m.source == g && c->getArgOperand(0) == v);
          auxiliary_locks.insert(c); used = true;
        }
      }
      need(used);
    }
  }
  std::string worker_frame(const State& s) {
    // Workers can only read immutable links and TOP payloads. Main SSA values,
    // main path constraints and payload samples cannot affect this projection.
    std::ostringstream out;
    for (const auto& entry : s.objects) {
      const auto& object = entry.second;
      out << entry.first << ':' << unsigned(object.place) << ':'
          << object.source << ':' << object.extent << ':' << object.born << ';';
    }
    out << '|';
    for (const auto& entry : s.links)
      out << entry.first.object << ':' << entry.first.offset << ':'
          << entry.first.width << ':' << unsigned(entry.second.place) << ':'
          << entry.second.object << ':' << entry.second.offset.to_string()
          << ';';
    out << '|';
    for (const auto& entry : s.scalars)
      out << entry.first.object << ':' << entry.first.offset << ':'
          << entry.first.width << ';';
    out << '|';
    for (auto index : s.initialized_mutexes)
      out << index << ';';
    out << '|';
    for (const auto& owner : s.owners)
      out << owner.first << ':' << owner.second << ';';
    return out.str();
  }
  State shared_worker_frame() {
    need(shared_graph && !worlds.empty());
    State merged = worlds.front();
    for (auto i = merged.objects.begin(); i != merged.objects.end();)
      if (i->second.place == Place::Heap) i = merged.objects.erase(i);
      else ++i;
    for (auto i = merged.scalars.begin(); i != merged.scalars.end();)
      if (!merged.objects.count(i->first.object)) i = merged.scalars.erase(i);
      else ++i;
    bool has_heap = false;
    for (const auto& world : worlds) {
      need(world.spawned && world.held.empty() && world.auxiliary_held.empty() &&
           world.initialized_mutexes == merged.initialized_mutexes);
      std::size_t ordinary_objects = 0;
      for (const auto& object : world.objects) {
        if (object.second.place == Place::Heap) {
          need(object.second.born && object.second.extent == node_extent);
          has_heap = true;
        } else {
          auto old = merged.objects.find(object.first);
          need(old != merged.objects.end() && old->second.place == object.second.place &&
               old->second.source == object.second.source && old->second.extent == object.second.extent &&
               old->second.born == object.second.born);
          ++ordinary_objects;
        }
      }
      need(ordinary_objects == merged.objects.size());
      for (const auto& cell : world.links) {
        spend();
        Key key = cell.first;
        const auto& owner = world.objects.at(key.object);
        if (owner.place == Place::Heap) key.object = 0;
        else need(owner.place == Place::Global && key.object == global_id);
        Pointer value = cell.second;
        value.offset = z.bv_val(constant(world, value.offset), 64);
        if (value.place == Place::Heap) value.object = 0;
        else need(value.place == Place::Global && value.object == global_id);
        auto& choices = shared_edges[key];
        if (std::none_of(choices.begin(), choices.end(), [&](const auto& previous) {
              return pointer_equal(previous, value);
            })) choices.push_back(value);
      }
    }
    merged.links.clear();
    merged.owners.clear();
    if (has_heap) {
      need(!merged.objects.count(0));
      merged.objects.emplace(0, Object{Place::Heap, nullptr, node_extent, true, true});
      // Presence records initialized bytes only. Every worker payload load is TOP.
      put(merged.scalars, Key{0, 0, datum_width},
          z.bv_val(0, node_type->getElementType(0)->getIntegerBitWidth()));
    }
    return merged;
  }
  bool has_access_record(const Endpoint& endpoint) {
    auto* statement = original(endpoint.statement);
    for (const auto& access : accesses) {
      if (access.stmt != statement || access.thread_id != endpoint.root->getName() ||
          access.width != endpoint.width || access.is_atomic ||
          (access.kind == AccessKind::Write) != endpoint.write) continue;
      std::vector< ar::CallBase* > calls;
      for (auto* c = access.call_context; !c->empty(); c = c->parent()) calls.push_back(c->call());
      std::reverse(calls.begin(), calls.end());
      if (calls == endpoint.calls) return true;
    }
    return false;
  }
  void prove() {
    inventory();
    State initial(z);
    initial.objects.emplace(global_id,
                            Object{Place::Global,
                                   global,
                                   extent(global->getValueType()),
                                   true});
    if (shared_graph)
      for (std::uint64_t bucket = 0; bucket < buckets; ++bucket)
        put(initial.scalars, Key{global_id, head_begin + bucket * head_stride, datum_width},
            z.bv_val(0, node_type->getElementType(0)->getIntegerBitWidth()));
    root = main;
    auto main_results = execute(main, std::move(initial), {});
    need(!main_results.empty() && !worlds.empty() && !workers.empty());
    const auto startup_world_count = worlds.size();
    for (const auto& r : main_results)
      need(r.state.held.empty() && r.state.auxiliary_held.empty());
    if (shared_graph) {
      State merged = shared_worker_frame();
      worlds.clear();
      worlds.push_back(std::move(merged));
    }
    std::set< std::string > projected;
    for (const auto& world : worlds) {
      need(world.spawned && world.held.empty() && world.auxiliary_held.empty());
      State frozen = world;
      for (auto& cell : frozen.links)
        cell.second.offset = z.bv_val(constant(world, cell.second.offset), 64);
      if (!projected.insert(worker_frame(frozen)).second)
        continue;
      for (auto* worker : workers) {
        State s = frozen;
        // Forgetting restrictions is an overapproximation. All main paths and
        // effects were already visited; link cells/identities stay unchanged.
        s.values.clear();
        s.pointers.clear();
        s.opaque.clear();
        s.path = z.bool_val(true);
        root = worker;
        context.clear();
        Argument arg;
        arg.pointer = Pointer(z);
        (void)execute(worker, std::move(s), {arg});
      }
    }
    log::debug(
        "Bucket-list complete startup worlds=" + std::to_string(startup_world_count) +
        ", distinct worker frames=" + std::to_string(projected.size()));
    if (shared_graph) {
      bool saw_payload = false;
      std::set< llvm::GlobalVariable* > common;
      for (const auto& statement : endpoints) {
        for (const auto& endpoint : statement.second) {
          if (endpoint.startup || !endpoint.payload) continue;
          need(has_access_record(endpoint));
          if (!saw_payload) {
            common = endpoint.auxiliary_held;
            saw_payload = true;
          } else {
            for (auto it = common.begin(); it != common.end();)
              if (!endpoint.auxiliary_held.count(*it)) it = common.erase(it);
              else ++it;
          }
        }
      }
      // Every source payload access is synchronized independently of AI addresses.
      shared_payload_closed = saw_payload && !common.empty();
    }
    // Every source statement observed by a later consumer has a complete role,
    // extent, initialized origin and context for all reaching executions.
    valid = true;
  }
  void ensure() {
    if (attempted)
      return;
    attempted = true;
    try {
      prove();
    } catch (const Refuse&) {
      valid = false;
    } catch (const z3::exception&) {
      valid = false;
    } catch (const std::out_of_range&) {
      valid = false;
    }
    if (!valid) {
      shared_payload_closed = false;
      endpoints.clear();
      original_mem_effects.clear();
      if (log::is_enabled_for(LogLevel::Debug)) {
        std::string text;
        llvm::raw_string_ostream stream(text);
        if (last)
          last->print(stream);
        log::debug("Bucket-list source proof incomplete at " + stream.str());
      }
    }
  }
  std::vector< const Endpoint* > matching(const AccessRecord& r) {
    std::vector< const Endpoint* > result;
    if (!r.stmt || !r.stmt->has_frontend() || !r.call_context)
      return result;
    auto* i =
        llvm::dyn_cast< llvm::Instruction >(r.stmt->frontend< llvm::Value >());
    if (!i || original(i) != r.stmt)
      return result;
    auto found = endpoints.find(i);
    if (found == endpoints.end())
      return result;
    std::vector< ar::CallBase* > cc;
    for (auto* c = r.call_context; !c->empty(); c = c->parent())
      cc.push_back(c->call());
    std::reverse(cc.begin(), cc.end());
    for (const auto& e : found->second)
      if (e.root->getName() == r.thread_id && e.calls == cc &&
          ((e.private_range && (r.width == word ||
              (r.width == 0 && llvm::isa< llvm::CallBase >(i) &&
               native_handle_api(llvm::cast< llvm::CallBase >(i), true)))) ||
           e.width == r.width ||
           (r.width == 0 && e.place == Place::Local && e.width == word &&
            llvm::isa< llvm::CallBase >(i) &&
            native_handle_api(llvm::cast< llvm::CallBase >(i), true))) &&
          e.write == (r.kind == AccessKind::Write))
        result.push_back(&e);
    return result;
  }
  bool pair(const Endpoint& a, const Endpoint& b) const {
    if (!a.write && !b.write)
      return true;
    if (a.startup || b.startup)
      return true;
    if (shared_graph && a.payload && b.payload)
      for (auto* mutex : a.auxiliary_held)
        if (b.auxiliary_held.count(mutex)) return true;
    if ((a.place == Place::Heap && b.place != Place::Heap) ||
        (b.place == Place::Heap && a.place != Place::Heap))
      return true;
    if (a.place == Place::Local || b.place == Place::Local)
      return a.object != b.object;
    if (a.place != Place::Heap || b.place != Place::Heap)
      return a.object != b.object || a.offset + a.width <= b.offset ||
             b.offset + b.width <= a.offset;
    if (shared_graph)
      return a.offset + a.width <= b.offset || b.offset + b.width <= a.offset;
    need(bool(a.owner) && bool(b.owner));
    if (a.owner != b.owner)
      return true;
    if (a.offset + a.width <= b.offset || b.offset + b.width <= a.offset)
      return true;
    return a.held && b.held; // one exact normal mutex at this owner index
  }

public:
  Impl(Context& c, const std::vector< AccessRecord >& records) : ctx(c), accesses(records) {}
  JsonDict info() const {
    return {{"bucket_list_contract", shared_graph ? "complete-typed-immutable-node-graph-v1"
                                                   : "complete-finite-closed-bucket-graph-v1"},
            {"mutex_model", "checked GNU/Linux x86 ILP32 or x86_64 LP64 default NORMAL; sealed ordinary runtime"},
            {"shared_payload_proof", shared_payload_closed
                ? "all-concurrent-payloads-share-sealed-normal-mutex" : "not-established"},
            {"scope", shared_graph
                ? (shared_payload_closed
                    ? "sealed MAY links; all concurrent payload accesses independently synchronized"
                    : "conditional typed byte footprints; sealed MAY links; overlapping payloads need ordinary race checks")
                : "conditional live node sources; complete CFG saturation; no pointer-validity, termination or BMC claim"}};
  }
  bool certifies(ar::Statement* statement, const std::string& reason) {
    if (!statement || !statement->has_frontend() ||
        (reason != "synthetic pthread handle representation not modeled" &&
         reason != "unknown destination write effects not covered"))
      return false;
    auto* i = llvm::dyn_cast< llvm::Instruction >(
        statement->frontend< llvm::Value >());
    if (!i || original(i) != statement) {
      ensure();
      if (!i || original(i) != statement)
        return false;
    }
    ensure();
    if (!valid) return false;
    // Unknown link writes need an independent complete payload proof; ordinary
    // AI's existing pointer summaries have no inclusion guarantee for them.
    if (shared_graph && reason == "unknown destination write effects not covered" &&
        isa< ar::Store >(statement) &&
        cast< ar::Store >(statement)->value()->type()->is_pointer() &&
        !shared_payload_closed)
      return false;
    if (reason == "synthetic pthread handle representation not modeled" &&
        covered_native_calls.count(statement)) return true;
    if (!original_mem_effects.count(i)) return false;
    if (reason == "unknown destination write effects not covered" &&
        !isa< ar::Store >(statement))
      return false;
    const auto& alternatives = endpoints.at(i);
    if (alternatives.empty())
      return false;
    for (const auto& e : alternatives) {
      if (e.place != Place::Heap && e.place != Place::Global)
        return false;
      if (e.write && !e.startup && !(e.payload && (shared_graph || (e.held && e.owner))))
        return false;
    }
    return true;
  }
  bool excludes(const AccessRecord& a, const AccessRecord& b) {
    ensure();
    if (!valid)
      return false;
    auto left = matching(a), right = matching(b);
    if (left.empty() || right.empty())
      return false;
    for (auto* x : left)
      for (auto* y : right)
        if (!pair(*x, *y))
          return false;
    return true;
  }
};
BucketListOwnership::BucketListOwnership(Context& c, const std::vector< AccessRecord >& records)
    : _impl(new Impl(c, records)) {}
BucketListOwnership::~BucketListOwnership() = default;
bool BucketListOwnership::certifies(ar::Statement* s, const std::string& r) {
  return _impl->certifies(s, r);
}
bool BucketListOwnership::excludes(const AccessRecord& a,
                                   const AccessRecord& b) {
  return _impl->excludes(a, b);
}
JsonDict BucketListOwnership::info() const {
  return _impl->info();
}
} // namespace analyzer
} // namespace ikos
