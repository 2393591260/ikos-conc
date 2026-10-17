// Copyright and license: see data_race.hpp.
#include <ikos/analyzer/checker/readonly_heap_reads.hpp>

#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/Dominators.h>
#include <z3++.h>
#include <algorithm>
#include <deque>
#include <functional>
#include <ikos/analyzer/analysis/source_call_binding.hpp>
#include <ikos/analyzer/checker/data_race.hpp>
#include <ikos/analyzer/util/log.hpp>
#include <map>
#include <set>

namespace ikos {
namespace analyzer {
namespace {
struct Refuse {};
template < typename Map, typename Value >
void put(Map& map, typename Map::key_type key, const Value& value) {
  auto i = map.find(key);
  if (i == map.end())
    map.emplace(key, value);
  else
    i->second = value;
}

enum class Mode { Main, Constructor, Worker, Scalar };
// Heap is nullable allocation provenance, never a non-NULL/liveness assertion.
enum class Place { Global, Local, Heap, Null };
struct Pointer {
  Place place;
  llvm::Value* object = nullptr;
  llvm::GlobalVariable* root = nullptr;
  llvm::Value* index = nullptr;
  std::uint64_t stride = 0;
  z3::expr offset, extent;
  bool initialized = false;
  unsigned snapshot = 0;
  std::shared_ptr< Pointer > row;
  Pointer(z3::context& z, Place p)
      : place(p), offset(z.bv_val(0, 64)), extent(offset) {}
};
struct State {
  std::map< llvm::Value*, z3::expr > values, cells;
  std::map< llvm::Value*, Pointer > pointers;
  z3::expr path;
  bool spawned = false;
  llvm::Function* worker = nullptr;
  unsigned atomic_depth = 0;
  bool scalar_snapshots = false;
  explicit State(z3::context& z) : path(z.bool_val(true)) {}
};
struct FunctionResult {
  State state;
  boost::optional< z3::expr > number;
  boost::optional< Pointer > pointer;
};
struct Job {
  State state;
  llvm::Instruction* pc;
  llvm::BasicBlock* predecessor = nullptr;
  boost::optional< z3::expr > low, iterator;
};
llvm::Function* target(llvm::CallBase* c) {
  return llvm::dyn_cast< llvm::Function >(
      c->getCalledOperand()->stripPointerCasts());
}
bool scalar(llvm::Type* t) {
  return t->isIntegerTy() && t->getIntegerBitWidth() <= 64;
}
llvm::Type* value_type(llvm::Value* v) {
  auto* g = llvm::dyn_cast< llvm::GlobalVariable >(v);
  return g ? g->getValueType() : v->getType();
}
} // namespace

class ReadonlyHeapReads::Impl {
  Context& ctx;
  z3::context z;
  llvm::Function* main = nullptr;
  std::map< llvm::Value*, ar::Statement* > originals;
  std::map< llvm::Function*, ar::Function* > functions;
  std::set< llvm::Function* > active, workers;
  std::vector< State > worlds;
  unsigned budget = 32768, queries = 2048, symbols = 0, world = 0;
  unsigned snapshots = 0;
  std::map< unsigned, z3::func_decl > reads;
  std::set< llvm::GlobalVariable* > mutable_globals;
  std::map< llvm::GlobalVariable*, std::set< llvm::Function* > > writers;
  std::set< llvm::GlobalVariable* > atomic_cells;
  std::set< llvm::Function* > unique_workers;
  std::set< llvm::Instruction* > certified;
  std::map< llvm::Instruction*, unsigned > certified_widths;
  bool attempted = false, valid = false;
  llvm::Instruction* last = nullptr;

  void spend() {
    if (!budget--)
      throw Refuse{};
  }
  z3::expr fresh(unsigned bits) {
    return z.bv_const(("readonly_" + std::to_string(++symbols)).c_str(), bits);
  }
  void set(State& s, llvm::Value* v, const z3::expr& x) {
    if (!scalar(value_type(v)) ||
        x.get_sort().bv_size() != value_type(v)->getIntegerBitWidth())
      throw Refuse{};
    put(s.values, v, x);
  }
  z3::expr number(const State& s, llvm::Value* v) {
    if (auto* n = llvm::dyn_cast< llvm::ConstantInt >(v)) {
      llvm::SmallString< 64 > text;
      n->getValue().toString(text, 10, false);
      return z.bv_val(text.c_str(), n->getBitWidth());
    }
    auto i = s.values.find(v);
    if (i == s.values.end() || !scalar(value_type(v)) ||
        i->second.get_sort().bv_size() != value_type(v)->getIntegerBitWidth())
      throw Refuse{};
    return i->second;
  }
  ar::Statement* original(llvm::Value* v) const {
    auto i = originals.find(v);
    return i == originals.end() ? nullptr : i->second;
  }
  ar::CallBase* binding(llvm::CallBase* c) const {
    return source_call_binding(c, original(c));
  }
  bool nondet(llvm::CallBase* c) const {
    auto* a = binding(c);
    auto* f = target(c);
    auto* t = a ? native_handle_callee(a) : nullptr;
    if (!t || !f->isDeclaration() || !c->arg_empty() || !scalar(c->getType()))
      return false;
    const auto name = f->getName();
    const unsigned bits = c->getType()->getIntegerBitWidth();
    if (name == "__VERIFIER_nondet_bool" && bits == 1 && t->has_frontend()) {
      auto* type = dyn_cast< ar::IntegerType >(t->type()->return_type());
      auto* result = a->result_or_null();
      auto* actual = result ? dyn_cast< ar::IntegerType >(result->type()) : nullptr;
      return type && actual && type->bit_width() == 1 && actual->bit_width() == 1;
    }
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
  bool atomic_marker(llvm::CallBase* c, bool begin) const {
    auto* a = binding(c);
    auto* f = target(c);
    auto* t = a ? native_handle_callee(a) : nullptr;
    return t && t->has_frontend() && f->isDeclaration() && c->arg_empty() &&
           c->getType()->isVoidTy() && f->getReturnType()->isVoidTy() &&
           f->getName() == (begin ? "__VERIFIER_atomic_begin"
                                  : "__VERIFIER_atomic_end");
  }
  bool owns(const State& s, llvm::GlobalVariable* g) const {
    auto i = writers.find(g);
    return s.scalar_snapshots && s.worker && unique_workers.count(s.worker) && i != writers.end() &&
           i->second.size() == 1 && *i->second.begin() == s.worker;
  }
  void forget_shared_cells(State& s) {
    for (auto* g : mutable_globals)
      if (!owns(s, g))
        set(s, g, fresh(g->getValueType()->getIntegerBitWidth()));
  }
  void scalar_inventory() {
    std::set< llvm::StoreInst* > protected_stores;
    for (auto& f : *main->getParent()) {
      if (f.isDeclaration() || &f == main)
        continue;
      std::map< llvm::BasicBlock*, unsigned > depths;
      std::deque< llvm::BasicBlock* > pending{&f.getEntryBlock()};
      depths.emplace(&f.getEntryBlock(), 0);
      while (!pending.empty()) {
        spend();
        auto* b = pending.front();
        pending.pop_front();
        unsigned depth = depths.at(b);
        for (auto& i : *b) {
          if (auto* call = llvm::dyn_cast< llvm::CallBase >(&i)) {
            if (atomic_marker(call, true)) {
              if (depth)
                throw Refuse{};
              depth = 1;
            } else if (atomic_marker(call, false)) {
              if (!depth)
                throw Refuse{};
              depth = 0;
            }
          }
          if (auto* st = llvm::dyn_cast< llvm::StoreInst >(&i))
            if (depth)
              protected_stores.insert(st);
        }
        for (auto* next : llvm::successors(b)) {
          auto inserted = depths.emplace(next, depth);
          if (inserted.second)
            pending.push_back(next);
          else if (inserted.first->second != depth)
            throw Refuse{};
        }
      }
      for (auto& b : f)
        for (auto& i : b)
          if (auto* st = llvm::dyn_cast< llvm::StoreInst >(&i))
            if (auto* g = llvm::dyn_cast< llvm::GlobalVariable >(
                    st->getPointerOperand())) {
              writers[g].insert(&f);
              if (!protected_stores.count(st))
                atomic_cells.erase(g);
            }
    }
    for (auto& f : *main->getParent()) {
      if (f.isDeclaration() || &f == main)
        continue;
      std::vector< llvm::Value* > pending{&f};
      std::set< llvm::Value* > seen;
      std::set< llvm::CallBase* > creates;
      bool closed = true;
      while (!pending.empty() && closed) {
        spend();
        auto* value = pending.back(); pending.pop_back();
        if (!seen.insert(value).second)
          continue;
        for (auto* use : value->users()) {
          if (llvm::isa< llvm::BitCastOperator >(use)) {
            pending.push_back(use);
            continue;
          }
          auto* call = llvm::dyn_cast< llvm::CallBase >(use);
          if (!call || call->getFunction() != main ||
              !native_handle_api(call, true) || !binding(call) ||
              call->getArgOperand(2)->stripPointerCasts() != &f) {
            closed = false;
            break;
          }
          creates.insert(call);
        }
      }
      // Main's complete CFG traversal separately refuses repeated create sites.
      if (closed && creates.size() == 1)
        unique_workers.insert(&f);
    }
  }
  void closed_scalar_effects(llvm::Function* root) {
    // Establish writer closure before cache-assisted path pruning.
    std::set< llvm::Function* > checking, done;
    std::function< void(llvm::Function*) > check = [&](llvm::Function* f) {
      if (done.count(f))
        return;
      if (!checking.insert(f).second || checking.size() > 16)
        throw Refuse{};
      for (auto& b : *f)
        for (auto& i : b) {
          spend();
          if (auto* st = llvm::dyn_cast< llvm::StoreInst >(&i)) {
            auto* g = llvm::dyn_cast< llvm::GlobalVariable >(st->getPointerOperand());
            auto* local = llvm::dyn_cast< llvm::AllocaInst >(st->getPointerOperand());
            auto* type = st->getValueOperand()->getType();
            if (st->isVolatile() || st->isAtomic() || !scalar(type) ||
                (g ? f != root || g->getValueType() != type
                   : !local || local->getAllocatedType() != type))
              throw Refuse{};
          } else if (auto* call = llvm::dyn_cast< llvm::CallBase >(&i)) {
            auto* called = target(call);
            if (!binding(call) || !called)
              throw Refuse{};
            if (nondet(call) || terminal(call))
              continue;
            if (atomic_marker(call, true) || atomic_marker(call, false)) {
              if (f != root)
                throw Refuse{};
              continue;
            }
            if (called->isDeclaration() ||
                (!called->getReturnType()->isVoidTy() && !scalar(called->getReturnType())))
              throw Refuse{};
            for (auto& arg : call->args())
              if (!scalar(arg->getType()))
                throw Refuse{};
            check(called);
          } else if (i.mayWriteToMemory()) {
            throw Refuse{};
          } else if (auto* ld = llvm::dyn_cast< llvm::LoadInst >(&i)) {
            if (ld->isVolatile() || ld->isAtomic())
              throw Refuse{};
          }
        }
      checking.erase(f);
      done.insert(f);
    };
    check(root);
  }
  bool feasible(const z3::expr& path) {
    if (!queries--)
      throw Refuse{};
    z3::solver solver(z);
    solver.set("timeout", 100u);
    solver.add(path);
    auto r = solver.check();
    if (r == z3::unknown)
      throw Refuse{};
    return r == z3::sat;
  }
  void require(const State& s, const z3::expr& predicate) {
    if (feasible(s.path && !predicate))
      throw Refuse{};
  }
  z3::expr extend(const z3::expr& v, bool sign = false) {
    if (v.get_sort().bv_size() > 64)
      throw Refuse{};
    return sign ? z3::sext(v, 64 - v.get_sort().bv_size())
                : z3::zext(v, 64 - v.get_sort().bv_size());
  }
  bool zero(const z3::expr& v) {
    return (v == z.bv_val(0, v.get_sort().bv_size())).simplify().is_true();
  }
  Pointer pointer(const State& s, llvm::Value* v) {
    if (llvm::isa< llvm::ConstantPointerNull >(v))
      return Pointer(z, Place::Null);
    if (auto* g = llvm::dyn_cast< llvm::GlobalVariable >(v)) {
      Pointer p(z, Place::Global);
      p.object = g;
      p.extent = z.bv_val(main->getParent()
                              ->getDataLayout()
                              .getTypeAllocSize(g->getValueType())
                              .getFixedValue(),
                          64);
      return p;
    }
    auto i = s.pointers.find(v);
    if (i == s.pointers.end())
      throw Refuse{};
    return i->second;
  }
  std::uint64_t bytes(llvm::Type* t) {
    if (!scalar(t) && !t->isPointerTy())
      throw Refuse{};
    return main->getParent()
        ->getDataLayout()
        .getTypeStoreSize(t)
        .getFixedValue();
  }
  bool same_index(ar::Value* v, llvm::Value* n) const {
    for (unsigned depth = 0; depth < 32; ++depth) {
      auto* type = dyn_cast< ar::IntegerType >(v->type());
      if (!type || type->bit_width() != 32 || !n->getType()->isIntegerTy(32))
        return false;
      auto* var = dyn_cast< ar::InternalVariable >(v);
      if (!var)
        return false;
      if (var->has_frontend() && var->frontend< llvm::Value >() == n)
        return true;
      auto* d = unique_def(var);
      if (auto* a = dyn_cast_or_null< ar::Assignment >(d))
        v = a->operand();
      else if (auto* a = dyn_cast_or_null< ar::UnaryOperation >(d)) {
        if (a->op() != ar::UnaryOperation::Bitcast)
          return false;
        v = a->operand();
      } else
        return false;
    }
    return false;
  }
  bool ar_index(ar::Value* v, llvm::Value* n, std::uint64_t stride) const {
    for (unsigned depth = 0; depth < 32; ++depth) {
      auto* var = dyn_cast< ar::InternalVariable >(v);
      auto* d = var ? unique_def(var) : nullptr;
      if (auto* p = dyn_cast_or_null< ar::PointerShift >(d)) {
        auto b = p->term_begin(), e = p->term_end();
        return b != e && (*b).first.to_z_number() == core::ZNumber(stride) &&
               same_index((*b).second, n) && ++b == e;
      }
      if (auto* a = dyn_cast_or_null< ar::Assignment >(d))
        v = a->operand();
      else if (auto* a = dyn_cast_or_null< ar::UnaryOperation >(d)) {
        if (a->op() != ar::UnaryOperation::Bitcast)
          return false;
        v = a->operand();
      } else
        return false;
    }
    return false;
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

  std::vector< FunctionResult > execute(
      llvm::Function* f,
      State initial,
      Mode mode,
      const std::vector< z3::expr >& arguments) {
    if (!active.insert(f).second || active.size() > 16 || f->isDeclaration())
      throw Refuse{};
    auto found = functions.find(f);
    if (found == functions.end())
      throw Refuse{};
    if (f->isVarArg() || f->getCallingConv() != llvm::CallingConv::C)
      throw Refuse{};
    for (unsigned n = 0; n < f->arg_size(); ++n)
      for (auto a : {llvm::Attribute::ByVal,
                     llvm::Attribute::StructRet,
                     llvm::Attribute::InAlloca,
                     llvm::Attribute::Preallocated})
        if (f->hasParamAttribute(n, a))
          throw Refuse{};
    for (auto& a : f->args()) {
      initial.values.erase(&a);
      initial.pointers.erase(&a);
    }
    for (auto& b : *f)
      for (auto& i : b) {
        initial.values.erase(&i);
        initial.pointers.erase(&i);
        initial.cells.erase(&i);
      }
    if (mode == Mode::Scalar || mode == Mode::Constructor) {
      if (f->arg_size() != arguments.size())
        throw Refuse{};
      for (unsigned n = 0; n < arguments.size(); ++n)
        set(initial, f->getArg(n), arguments[n]);
    } else if (mode == Mode::Worker) {
      if (f->arg_size() != 1 || !f->getArg(0)->getType()->isPointerTy() ||
          !f->getReturnType()->isPointerTy())
        throw Refuse{};
      initial.spawned = true;
      initial.worker = f;
      for (auto* g : mutable_globals) {
        if (!scalar(g->getValueType()))
          throw Refuse{};
        set(initial, g, fresh(g->getValueType()->getIntegerBitWidth()));
      }
      initial.pointers.emplace(f->getArg(0), Pointer(z, Place::Null));
    } else {
      for (auto& a : f->args())
        if (!a.use_empty())
          throw Refuse{};
    }
    llvm::DominatorTree dom(*f);
    llvm::LoopInfo loops(dom);
    llvm::Loop* loop = nullptr;
    llvm::PHINode* phi = nullptr;
    llvm::Value* upper = nullptr;
    llvm::BasicBlock* latch = nullptr;
    llvm::CallBase* allocation = nullptr;
    std::uint64_t stride = 0;
    llvm::StoreInst* prefix_store = nullptr;
    llvm::CallBase* row_constructor = nullptr;
    llvm::GlobalVariable* table_global = nullptr;
    bool arbitrary_header = false;
    const unsigned entering_atomic_depth = initial.atomic_depth;
    if (mode == Mode::Constructor || mode == Mode::Worker ||
        (mode == Mode::Main && !loops.empty())) {
      if (std::distance(loops.begin(), loops.end()) != 1)
        throw Refuse{};
      loop = *loops.begin();
      latch = loop->getLoopLatch();
      if (!loop->getSubLoops().empty() || !latch || !loop->getLoopPreheader() ||
          loop->getExitingBlock() != loop->getHeader() || !loop->getExitBlock())
        throw Refuse{};
      for (auto& i : *loop->getHeader())
        if (auto* p = llvm::dyn_cast< llvm::PHINode >(&i)) {
          if (phi || !p->getType()->isIntegerTy(32) ||
              p->getNumIncomingValues() != 2) {
            if (mode != Mode::Worker || !scalar(p->getType()))
              throw Refuse{};
            arbitrary_header = true;
          }
          phi = p;
        }
      auto* branch = llvm::dyn_cast< llvm::BranchInst >(
          loop->getHeader()->getTerminator());
      auto* compare =
          branch && branch->isConditional()
              ? llvm::dyn_cast< llvm::ICmpInst >(branch->getCondition())
              : nullptr;
      auto* step = phi ? llvm::dyn_cast< llvm::BinaryOperator >(
                             phi->getIncomingValueForBlock(latch))
                       : nullptr;
      auto* one = step
                      ? llvm::dyn_cast< llvm::ConstantInt >(step->getOperand(1))
                      : nullptr;
      if (!phi || !compare || arbitrary_header ||
          compare->getPredicate() != llvm::CmpInst::ICMP_SLT ||
          compare->getOperand(0) != phi ||
          !loop->contains(branch->getSuccessor(0)) ||
          loop->contains(branch->getSuccessor(1)) || !step ||
          step->getOpcode() != llvm::Instruction::Add ||
          step->getOperand(0) != phi || !one || !one->isOne() ||
          !dom.dominates(branch->getSuccessor(0), latch)) {
        if (mode != Mode::Worker)
          throw Refuse{};
        arbitrary_header = true;
      }
      if (!arbitrary_header) {
        upper = compare->getOperand(1);
        // A changing heap value or nondet loop bound is not a stable interval
        // end.
        std::set< llvm::Value* > seen;
        std::function< bool(llvm::Value*) > stable = [&](llvm::Value* v) {
          if (!seen.insert(v).second)
            return false;
          if (llvm::isa< llvm::ConstantInt >(v) || llvm::isa< llvm::Argument >(v))
            return true;
          if (auto* load = llvm::dyn_cast< llvm::LoadInst >(v)) {
            auto* g =
                llvm::dyn_cast< llvm::GlobalVariable >(load->getPointerOperand());
            return g && !mutable_globals.count(g);
          }
          if (auto* cast = llvm::dyn_cast< llvm::CastInst >(v))
            return stable(cast->getOperand(0));
          return false;
        };
        if (!stable(upper))
          throw Refuse{};
      } else {
        phi = nullptr;
      }
    } else if (!loops.empty())
      throw Refuse{};
    // Check the entire CFG for cycles beyond the one certified backedge.
    std::set< llvm::BasicBlock* > visiting, done;
    std::function< void(llvm::BasicBlock*) > acyclic =
        [&](llvm::BasicBlock* b) {
          spend();
          if (visiting.count(b))
            throw Refuse{};
          if (done.count(b))
            return;
          visiting.insert(b);
          for (auto* next : llvm::successors(b)) {
            if (loop && b == latch && next == loop->getHeader())
              continue;
            acyclic(next);
          }
          visiting.erase(b);
          done.insert(b);
        };
    acyclic(&f->getEntryBlock());
    if (mode == Mode::Constructor) {
      auto* returned = llvm::dyn_cast< llvm::PointerType >(f->getReturnType());
      if (f->arg_size() != 1 || !f->getArg(0)->getType()->isIntegerTy(32) ||
          !returned || returned->getAddressSpace() ||
          !scalar(returned->getPointerElementType()))
        throw Refuse{};
      stride = bytes(returned->getPointerElementType());
      unsigned returns = 0;
      for (auto& b : *f)
        for (auto& i : b)
          if (auto* call = llvm::dyn_cast< llvm::CallBase >(&i)) {
            auto* a = binding(call);
            if (a && native_handle_callee(a)->intrinsic_id() ==
                         ar::Intrinsic::LibcMalloc) {
              if (allocation || loop->contains(&i))
                throw Refuse{};
              allocation = call;
            }
          }
      if (!allocation)
        throw Refuse{};
      for (auto& b : *f)
        if (auto* r = llvm::dyn_cast< llvm::ReturnInst >(b.getTerminator())) {
          if (!r->getReturnValue() ||
              r->getReturnValue()->stripPointerCasts() != allocation ||
              !dom.dominates(allocation, r) ||
              !dom.dominates(loop->getExitBlock(), r->getParent()))
            throw Refuse{};
          ++returns;
        }
      if (!returns)
        throw Refuse{};
    }
    if (loop && (mode == Mode::Constructor || mode == Mode::Main)) {
      auto* start = llvm::dyn_cast< llvm::ConstantInt >(
          phi->getIncomingValueForBlock(loop->getLoopPreheader()));
      if (!start || !start->isZero())
        throw Refuse{};
      if (mode == Mode::Constructor && upper != f->getArg(0))
        throw Refuse{};
      for (auto* b : loop->blocks())
        for (auto& i : *b) {
          if (auto* st = llvm::dyn_cast< llvm::StoreInst >(&i)) {
            if (prefix_store || !dom.dominates(st, latch->getTerminator()))
              throw Refuse{};
            prefix_store = st;
          }
        }
      if (!prefix_store)
        throw Refuse{};
      auto* gep = llvm::dyn_cast< llvm::GetElementPtrInst >(
          prefix_store->getPointerOperand()->stripPointerCasts());
      if (!gep || gep->getNumIndices() != 1 || gep->getOperand(1) != phi)
        throw Refuse{};
      if (mode == Mode::Constructor) {
        if (gep->getPointerOperand()->stripPointerCasts() != allocation ||
            !scalar(prefix_store->getValueOperand()->getType()) ||
            bytes(prefix_store->getValueOperand()->getType()) != stride)
          throw Refuse{};
      } else {
        auto* base = llvm::dyn_cast< llvm::LoadInst >(
            gep->getPointerOperand()->stripPointerCasts());
        table_global = base ? llvm::dyn_cast< llvm::GlobalVariable >(
                                  base->getPointerOperand())
                            : nullptr;
        row_constructor = llvm::dyn_cast< llvm::CallBase >(
            prefix_store->getValueOperand()->stripPointerCasts());
        if (!table_global || !row_constructor ||
            !loop->contains(row_constructor) || !binding(row_constructor) ||
            !target(row_constructor) ||
            target(row_constructor)->isDeclaration() ||
            !dom.dominates(row_constructor, prefix_store) ||
            row_constructor->arg_size() != 1 ||
            !row_constructor->getArgOperand(0)->getType()->isIntegerTy(32) ||
            !prefix_store->getValueOperand()->getType()->isPointerTy())
          throw Refuse{};
        stride = bytes(prefix_store->getValueOperand()->getType());
      }
    }
    std::deque< Job > pending;
    if (arbitrary_header) {
      initial.scalar_snapshots = true;
      closed_scalar_effects(f);
      // Every iteration starts in this overapproximation, independently of
      // worker entry guards. Immutable startup sources alone are retained.
      State header = initial;
      if (header.atomic_depth)
        throw Refuse{};
      for (auto& b : *f)
        for (auto& i : b) {
          if (scalar(i.getType()))
            set(header, &i, fresh(i.getType()->getIntegerBitWidth()));
          header.pointers.erase(&i);
          header.cells.erase(&i);
        }
      for (auto* g : mutable_globals)
        set(header, g, fresh(g->getValueType()->getIntegerBitWidth()));
      pending.push_back(
          {std::move(header), &loop->getHeader()->front(), nullptr, {}, {}});
    }
    pending.push_back(
        {std::move(initial), &f->getEntryBlock().front(), nullptr, {}, {}});
    std::vector< FunctionResult > results;
    while (!pending.empty()) {
      spend();
      auto j = std::move(pending.front());
      pending.pop_front();
      auto& s = j.state;
      auto* i = j.pc;
      if (!i)
        throw Refuse{};
      last = i;
      if (loop && !arbitrary_header && i == &loop->getHeader()->front() &&
          j.predecessor == loop->getLoopPreheader()) {
        j.low = number(s, phi->getIncomingValueForBlock(j.predecessor));
        if (mode == Mode::Main) {
          if (s.spawned)
            throw Refuse{};
          auto table_it = s.pointers.find(table_global);
          if (table_it == s.pointers.end() ||
              table_it->second.place != Place::Heap)
            throw Refuse{};
          auto& table = table_it->second;
          auto* table_allocation =
              llvm::dyn_cast< llvm::CallBase >(table.object);
          auto* allocation_binding =
              table_allocation ? binding(table_allocation) : nullptr;
          // Do not overwrite a scalar snapshot through a retyped table alias.
          if (!table_allocation || table_allocation->getFunction() != main ||
              !allocation_binding ||
              native_handle_callee(allocation_binding)->intrinsic_id() !=
                  ar::Intrinsic::LibcMalloc ||
              !zero(table.offset) || table.initialized || table.row)
            throw Refuse{};
          if (auto* bound = llvm::dyn_cast< llvm::LoadInst >(upper)) {
            auto* global = llvm::dyn_cast< llvm::GlobalVariable >(
                bound->getPointerOperand());
            if (!global || mutable_globals.count(global))
              throw Refuse{};
            set(s, bound, number(s, global));
          }
          auto count = number(s, upper);
          require(s,
                  count >= z.bv_val(0, 32) &&
                      table.extent == extend(count) * z.bv_val(stride, 64) &&
                      extend(count) * z.bv_val(stride, 64) <=
                          z.bv_val(UINT64_C(0xffffffff), 64));
          auto* argument = row_constructor->getArgOperand(0);
          // The row length may be a loop-local whole scalar global reload.
          auto* ld = llvm::dyn_cast< llvm::LoadInst >(argument);
          if (ld && loop->contains(ld)) {
            auto* global =
                llvm::dyn_cast< llvm::GlobalVariable >(ld->getPointerOperand());
            if (!global || mutable_globals.count(global))
              throw Refuse{};
            set(s, ld, number(s, global));
          }
          auto rows = execute(target(row_constructor),
                              s,
                              Mode::Constructor,
                              {number(s, argument)});
          if (rows.empty())
            throw Refuse{};
          z3::expr successful = z.bool_val(false);
          boost::optional< Pointer > row;
          for (auto& r : rows) {
            if (!r.pointer || !r.pointer->initialized || r.pointer->row)
              throw Refuse{};
            if (row && (row->stride != r.pointer->stride ||
                        !z3::eq(row->extent, r.pointer->extent)))
              throw Refuse{};
            row = *r.pointer;
            successful = successful || r.state.path;
          }
          // No row exists when count==0; constructor guards are conditional.
          s.path = s.path && (count == z.bv_val(0, 32) || successful);
          table.stride = stride;
          table.row = std::make_shared< Pointer >(*row);
          table.initialized = false;
        }
        j.iterator = fresh(32);
        set(s, phi, *j.iterator);
        s.path = s.path && *j.iterator >= *j.low;
      }
      if (auto* p = llvm::dyn_cast< llvm::PHINode >(i)) {
        if (arbitrary_header && p->getParent() == loop->getHeader() &&
            !j.predecessor) {
          if (!scalar(p->getType()))
            throw Refuse{};
        } else if (p != phi) {
          auto* v = p->getIncomingValueForBlock(j.predecessor);
          if (scalar(p->getType()))
            set(s, p, number(s, v));
          else if (p->getType()->isPointerTy())
            put(s.pointers, p, pointer(s, v));
          else
            throw Refuse{};
        }
      } else if (auto* branch = llvm::dyn_cast< llvm::BranchInst >(i)) {
        for (unsigned n = 0; n < branch->getNumSuccessors(); ++n) {
          Job child = j;
          if (branch->isConditional())
            child.state.path =
                child.state.path && (number(s, branch->getCondition()) ==
                                     z.bv_val(n == 0 ? 1 : 0, 1));
          if (!feasible(child.state.path))
            continue;
          auto* next = branch->getSuccessor(n);
          if (arbitrary_header && next == loop->getHeader()) {
            if (child.state.atomic_depth)
              throw Refuse{};
            continue;
          }
          if (loop && i->getParent() == latch && next == loop->getHeader()) {
            auto increment = number(s, phi->getIncomingValueForBlock(latch));
            require(child.state,
                    increment > *j.iterator && increment >= *j.low);
            continue;
          }
          if (mode == Mode::Main && loop &&
              i->getParent() == loop->getHeader() &&
              next == loop->getExitBlock())
            child.state.pointers.at(table_global).initialized = true;
          child.pc = &next->front();
          child.predecessor = i->getParent();
          pending.push_back(std::move(child));
        }
        continue;
      } else if (auto* ret = llvm::dyn_cast< llvm::ReturnInst >(i)) {
        if (s.atomic_depth != entering_atomic_depth)
          throw Refuse{};
        FunctionResult r{std::move(s), {}, {}};
        if (auto* v = ret->getReturnValue()) {
          if (scalar(v->getType()))
            r.number = number(r.state, v);
          else if (v->getType()->isPointerTy()) {
            r.pointer = pointer(r.state, v);
            if (mode == Mode::Constructor)
              r.pointer->initialized = true;
          } else
            throw Refuse{};
        }
        results.push_back(std::move(r));
        continue;
      } else if (llvm::isa< llvm::UnreachableInst >(i)) {
        throw Refuse{};
      } else if (auto* alloca = llvm::dyn_cast< llvm::AllocaInst >(i)) {
        auto* a = dyn_cast_or_null< ar::Allocate >(original(i));
        auto* count =
            llvm::dyn_cast< llvm::ConstantInt >(alloca->getArraySize());
        if (!a || !count || !count->isOne() ||
            !scalar(alloca->getAllocatedType()) || (loop && loop->contains(i)))
          throw Refuse{};
        Pointer p(z, Place::Local);
        p.object = alloca;
        p.extent = z.bv_val(bytes(alloca->getAllocatedType()), 64);
        put(s.pointers, i, p);
      } else if (auto* gep = llvm::dyn_cast< llvm::GetElementPtrInst >(i)) {
        auto base_pointer = pointer(s, gep->getPointerOperand());
        if (base_pointer.place == Place::Global &&
            llvm::cast< llvm::GlobalVariable >(base_pointer.object)
                ->isConstant() &&
            gep->hasAllConstantIndices()) {
          llvm::APInt displacement(32, 0);
          if (!gep->accumulateConstantOffset(main->getParent()->getDataLayout(),
                                             displacement))
            throw Refuse{};
          base_pointer.offset =
              base_pointer.offset + z.bv_val(displacement.getSExtValue(), 64);
          require(s,
                  base_pointer.offset >= z.bv_val(0, 64) &&
                      base_pointer.offset <= base_pointer.extent);
          put(s.pointers, i, base_pointer);
          j.pc = i->getNextNode();
          pending.push_back(std::move(j));
          continue;
        }
        if (gep->getNumIndices() != 1 ||
            !gep->getOperand(1)->getType()->isIntegerTy(32) ||
            (!scalar(gep->getSourceElementType()) &&
             !gep->getSourceElementType()->isPointerTy()))
          throw Refuse{};
        auto p = pointer(s, gep->getPointerOperand());
        const auto step = bytes(gep->getSourceElementType());
        if (p.place != Place::Heap || step != p.stride)
          throw Refuse{};
        const bool direct = p.root && p.offset.simplify().is_numeral() &&
                            p.offset.simplify().get_numeral_uint64() == 0;
        p.offset = p.offset + extend(number(s, gep->getOperand(1)), true) *
                                  z.bv_val(step, 64);
        p.index = direct ? gep->getOperand(1) : nullptr;
        put(s.pointers, i, p);
      } else if (auto* conversion = llvm::dyn_cast< llvm::BitCastInst >(i)) {
        if (conversion->getType()->isPointerTy() &&
            conversion->getOperand(0)->getType()->isPointerTy()) {
          auto p = pointer(s, conversion->getOperand(0));
          auto* element = conversion->getType()->getPointerElementType();
          if (p.place == Place::Heap && !p.stride &&
              (scalar(element) || element->isPointerTy()))
            p.stride = bytes(element);
          put(s.pointers, i, p);
        } else
          set(s, i, operation(s, i));
      } else if (llvm::isa< llvm::LoadInst >(i) ||
                 llvm::isa< llvm::StoreInst >(i)) {
        auto* load = llvm::dyn_cast< llvm::LoadInst >(i);
        auto* store = llvm::dyn_cast< llvm::StoreInst >(i);
        auto* a = original(i);
        if ((load &&
             (!isa< ar::Load >(a) || load->isAtomic() || load->isVolatile())) ||
            (store && (!isa< ar::Store >(a) || store->isAtomic() ||
                       store->isVolatile())))
          throw Refuse{};
        auto p = pointer(s,
                         load ? load->getPointerOperand()
                              : store->getPointerOperand());
        auto* type =
            load ? load->getType() : store->getValueOperand()->getType();
        const auto width = bytes(type);
        require(s,
                p.offset >= z.bv_val(0, 64) &&
                    p.offset <= z.bv_val(UINT64_C(0xffffffff), 64) &&
                    p.offset + z.bv_val(width, 64) <= p.extent);
        if (p.place == Place::Global) {
          auto* g = llvm::cast< llvm::GlobalVariable >(p.object);
          if ((store && mode == Mode::Worker &&
               store->getPointerOperand() != g) ||
              mode == Mode::Constructor || mode == Mode::Scalar ||
              (store &&
               (type->isPointerTy() && (mode != Mode::Main || s.spawned))) ||
              (store && mode == Mode::Main && s.spawned) || !zero(p.offset) ||
              type != g->getValueType() || width != bytes(g->getValueType()))
            throw Refuse{};
          if (type->isPointerTy()) {
            if (load) {
              auto q = s.pointers.at(g);
              q.root = g;
              q.index = nullptr;
              put(s.pointers, i, q);
            } else {
              auto q = pointer(s, store->getValueOperand());
              if (q.place != Place::Heap)
                throw Refuse{};
              put(s.pointers, g, q);
            }
          } else if (load)
            set(s,
                i,
                (s.spawned && mutable_globals.count(g) &&
                 !owns(s, g) && !(s.scalar_snapshots && s.worker &&
                                  s.atomic_depth && atomic_cells.count(g)))
                    ? fresh(type->getIntegerBitWidth())
                    : number(s, g));
          else
            set(s, g, number(s, store->getValueOperand()));
        } else if (p.place == Place::Local) {
          if (!zero(p.offset) || !scalar(type) || (loop && loop->contains(i)) ||
              llvm::cast< llvm::AllocaInst >(p.object)->getAllocatedType() !=
                  type ||
              !p.extent.simplify().is_numeral() ||
              p.extent.simplify().get_numeral_uint64() != width)
            throw Refuse{};
          if (load) {
            auto old = s.cells.find(p.object);
            set(s,
                i,
                old == s.cells.end() ? fresh(type->getIntegerBitWidth())
                                     : old->second);
          } else
            put(s.cells, p.object, number(s, store->getValueOperand()));
        } else if (p.place == Place::Heap) {
          if (width != p.stride || (!scalar(type) && !type->isPointerTy()) ||
              (mode == Mode::Worker && store) ||
              (mode == Mode::Main && s.spawned && store) ||
              (store && store != prefix_store) || (load && !p.initialized))
            throw Refuse{};
          auto* ar_ptr = load ? cast< ar::Load >(a)->operand()
                              : cast< ar::Store >(a)->pointer();
          auto* address =
              load ? load->getPointerOperand() : store->getPointerOperand();
          auto* gep = llvm::dyn_cast< llvm::GetElementPtrInst >(
              address->stripPointerCasts());
          auto* ar_type = load ? cast< ar::Load >(a)->result()->type()
                               : cast< ar::Store >(a)->value()->type();
          if (!gep || gep->getNumIndices() != 1 ||
              ctx.bundle->data_layout().store_size_in_bytes(ar_type) !=
                  core::ZNumber(width) ||
              !ar_index(ar_ptr, gep->getOperand(1), p.stride))
            throw Refuse{};
          if (store) {
            require(s,
                    p.offset ==
                        extend(number(s, phi)) * z.bv_val(p.stride, 64));
            if (type->isPointerTy()) {
              auto row = pointer(s, store->getValueOperand());
              if (mode != Mode::Main || row.place != Place::Heap ||
                  !row.initialized || row.row || !zero(row.offset) || !p.row ||
                  p.row->stride != row.stride)
                throw Refuse{};
              require(s, p.row->extent == row.extent);
            } else
              (void)number(s, store->getValueOperand());
          } else if (type->isPointerTy()) {
            if (!p.row || !p.row->initialized || p.row->row)
              throw Refuse{};
            auto row = *p.row;
            row.root = nullptr;
            row.index = nullptr;
            put(s.pointers, i, row);
          } else if (!p.root) {
            // Rows may alias, but treating their payload loads independently
            // overapproximates all such aliasing without conflating row cells.
            set(s, i, fresh(type->getIntegerBitWidth()));
          } else {
            if (p.row)
              throw Refuse{};
            auto fun = reads.find(p.snapshot);
            if (fun == reads.end()) {
              auto name = "readonly_cell_" + std::to_string(p.snapshot);
              fun = reads
                        .emplace(p.snapshot,
                                 z.function(name.c_str(),
                                            z.bv_sort(64),
                                            z.bv_sort(
                                                type->getIntegerBitWidth())))
                        .first;
            }
            if (fun->second.range().bv_size() != type->getIntegerBitWidth())
              throw Refuse{};
            set(s, i, fun->second(p.offset));
          }
          if (mode == Mode::Worker && load) {
            certified.insert(i);
            certified_widths[i] = width;
          }
        } else
          throw Refuse{};
      } else if (auto* call = llvm::dyn_cast< llvm::CallBase >(i)) {
        auto* a = binding(call);
        auto* called = target(call);
        if (!a || !called)
          throw Refuse{};
        if (terminal(call))
          continue;
        if (nondet(call))
          set(s, i, fresh(call->getType()->getIntegerBitWidth()));
        else if (!called->isDeclaration()) {
          std::vector< z3::expr > args;
          for (auto& arg : call->args()) {
            if (!scalar(arg->getType()))
              throw Refuse{};
            args.push_back(number(s, arg.get()));
          }
          Mode child_mode = called->getReturnType()->isPointerTy()
                                ? Mode::Constructor
                                : Mode::Scalar;
          if (child_mode == Mode::Constructor && mode != Mode::Main)
            throw Refuse{};
          auto children = execute(called, s, child_mode, args);
          for (auto& result : children) {
            Job next = j;
            next.state = std::move(result.state);
            next.pc = i->getNextNode();
            if (scalar(call->getType())) {
              if (!result.number)
                throw Refuse{};
              set(next.state, i, *result.number);
            } else if (call->getType()->isPointerTy()) {
              if (!result.pointer)
                throw Refuse{};
              put(next.state.pointers, i, *result.pointer);
            } else if (!call->getType()->isVoidTy())
              throw Refuse{};
            pending.push_back(std::move(next));
          }
          continue;
        } else if (native_handle_callee(a)->intrinsic_id() ==
                   ar::Intrinsic::LibcMalloc) {
          if ((mode != Mode::Constructor && mode != Mode::Main) ||
              (mode == Mode::Constructor && call != allocation) || s.spawned ||
              call->arg_size() != 1 || called->getName() != "malloc" ||
              !call->getType()->isPointerTy() ||
              !call->getArgOperand(0)->getType()->isIntegerTy(32))
            throw Refuse{};
          auto size = number(s, call->getArgOperand(0));
          if (mode == Mode::Constructor) {
            auto length = number(s, f->getArg(0));
            require(s,
                    length >= z.bv_val(0, 32) &&
                        extend(size) == extend(length) * z.bv_val(stride, 64) &&
                        extend(length) * z.bv_val(stride, 64) <=
                            z.bv_val(UINT64_C(0xffffffff), 64));
          }
          Pointer p(z, Place::Heap);
          p.object = call;
          p.stride = mode == Mode::Constructor ? stride : 0;
          p.snapshot = ++snapshots;
          p.extent = extend(size);
          put(s.pointers, i, p);
        } else if (atomic_marker(call, true) || atomic_marker(call, false)) {
          if (mode != Mode::Worker)
            throw Refuse{};
          const bool begin = atomic_marker(call, true);
          if (s.atomic_depth != (begin ? 0u : 1u))
            throw Refuse{};
          forget_shared_cells(s);
          s.atomic_depth = begin ? 1u : 0u;
        } else if (native_handle_api(call, true)) {
          if (mode != Mode::Main ||
              (loop &&
               (loop->contains(call) ||
                !dom.dominates(loop->getExitBlock(), call->getParent()))) ||
              !llvm::isa< llvm::ConstantPointerNull >(call->getArgOperand(1)) ||
              !llvm::isa< llvm::ConstantPointerNull >(call->getArgOperand(3)))
            throw Refuse{};
          auto p = pointer(s, call->getArgOperand(0));
          auto* child = llvm::dyn_cast< llvm::Function >(
              call->getArgOperand(2)->stripPointerCasts());
          if (p.place != Place::Local || !zero(p.offset) ||
              !p.extent.simplify().is_numeral() ||
              p.extent.simplify().get_numeral_uint64() != 4 || !child ||
              child == main || child->isDeclaration())
            throw Refuse{};
          workers.insert(child);
          if (!s.spawned) {
            s.spawned = true;
            worlds.push_back(s);
          }
          put(s.cells, p.object, fresh(32));
          set(s, i, fresh(32));
        } else if (native_handle_api(call, false)) {
          if (mode != Mode::Main ||
              !llvm::isa< llvm::ConstantPointerNull >(call->getArgOperand(1)))
            throw Refuse{};
          (void)number(s, call->getArgOperand(0));
          set(s, i, fresh(32));
        } else
          throw Refuse{};
      } else if (scalar(i->getType()))
        set(s, i, operation(s, i));
      else
        throw Refuse{};
      j.pc = i->getNextNode();
      pending.push_back(std::move(j));
    }
    active.erase(f);
    return results;
  }

  void prove() {
    if (!ctx.opts.enable_thread_modular ||
        ctx.opts.procedural == Procedural::Intraprocedural ||
        ctx.opts.entry_points.size() != 1 ||
        ctx.opts.entry_points.front()->name() != "main" ||
        !ctx.opts.entry_points.front()->has_frontend() ||
        ctx.bundle->data_layout().pointers.bit_width != 32 ||
        !ctx.opts.no_init_globals.empty() ||
        ctx.bundle->global_or_null("ar.global_ctors") ||
        ctx.bundle->global_or_null("ar.global_dtors"))
      throw Refuse{};
    main = ctx.opts.entry_points.front()->frontend< llvm::Function >();
    if (!main->use_empty() || !main->getReturnType()->isIntegerTy(32) ||
        main->getParent()->getDataLayout().getPointerSizeInBits(0) != 32 ||
        !main->getParent()->getModuleInlineAsm().empty() ||
        main->getParent()->getNamedGlobal("llvm.global_ctors") ||
        main->getParent()->getNamedGlobal("llvm.global_dtors"))
      throw Refuse{};
    for (auto it = ctx.bundle->function_begin();
         it != ctx.bundle->function_end();
         ++it) {
      if (!(*it)->is_definition())
        continue;
      if (!(*it)->has_frontend())
        throw Refuse{};
      auto* f = (*it)->frontend< llvm::Function >();
      functions.emplace(f, *it);
      for (auto* b : *(*it)->body())
        for (auto* s : *b) {
          spend();
          if (isa< ar::CallBase >(s) || isa< ar::Load >(s) ||
              isa< ar::Store >(s) || isa< ar::Allocate >(s)) {
            auto* n = s->has_frontend() ? llvm::dyn_cast< llvm::Instruction >(
                                              s->frontend< llvm::Value >())
                                        : nullptr;
            if (!n || n->getFunction() != f || !originals.emplace(n, s).second)
              throw Refuse{};
          }
        }
    }
    for (auto& fn : *main->getParent())
      if (&fn != main && !fn.isDeclaration())
        for (auto& b : fn)
          for (auto& i : b)
            if (auto* st = llvm::dyn_cast< llvm::StoreInst >(&i))
              if (auto* g = llvm::dyn_cast< llvm::GlobalVariable >(
                      st->getPointerOperand()))
                mutable_globals.insert(g);
    atomic_cells = mutable_globals;
    scalar_inventory();
    State initial(z);
    for (auto& g : main->getParent()->globals()) {
      if (g.isThreadLocal() || g.getAddressSpace() ||
          g.isExternallyInitialized() || !g.hasInitializer())
        throw Refuse{};
      if (auto* n = llvm::dyn_cast< llvm::ConstantInt >(g.getInitializer()))
        set(initial, &g, number(initial, n));
      else if (llvm::isa< llvm::ConstantPointerNull >(g.getInitializer()))
        initial.pointers.emplace(&g, Pointer(z, Place::Null));
      else if (!g.isConstant())
        throw Refuse{};
    }
    (void)execute(main, std::move(initial), Mode::Main, {});
    if (worlds.empty() || workers.empty())
      throw Refuse{};
    std::vector< llvm::Function* > ordered(workers.begin(), workers.end());
    std::sort(ordered.begin(), ordered.end(), [](auto* a, auto* b) {
      return a->getName() < b->getName();
    });
    for (world = 0; world < worlds.size(); ++world)
      for (auto* f : ordered)
        (void)execute(f, worlds[world], Mode::Worker, {});
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
      certified.clear();
      certified_widths.clear();
      if (log::is_enabled_for(LogLevel::Debug)) {
        std::string text;
        llvm::raw_string_ostream stream(text);
        if (last)
          last->print(stream);
        log::debug("Readonly heap reads: incomplete frame or induction at " +
                   stream.str());
      }
    }
  }
  bool direct_global(const AccessRecord& r) const {
    if (!r.stmt || !r.stmt->has_frontend() || !r.call_context ||
        !r.call_context->empty() || !r.width)
      return false;
    auto* i =
        llvm::dyn_cast< llvm::Instruction >(r.stmt->frontend< llvm::Value >());
    auto* st = llvm::dyn_cast_or_null< llvm::StoreInst >(i);
    if (!st || r.kind != AccessKind::Write || st->isAtomic() ||
        st->isVolatile())
      return false;
    auto* g = llvm::dyn_cast< llvm::GlobalVariable >(st->getPointerOperand());
    auto* type = st->getValueOperand()->getType();
    return g && scalar(type) && type == g->getValueType() &&
           !g->isThreadLocal() && original(i) == r.stmt &&
           r.width == main->getParent()
                          ->getDataLayout()
                          .getTypeStoreSize(type)
                          .getFixedValue() &&
           r.thread_id == i->getFunction()->getName().str();
  }

public:
  explicit Impl(Context& c) : ctx(c) {}
  bool certifies(ar::Statement* stmt) {
    if (!stmt || !isa< ar::Load >(stmt) || !stmt->has_frontend())
      return false;
    auto* ld =
        llvm::dyn_cast< llvm::LoadInst >(stmt->frontend< llvm::Value >());
    if (!ld || ld->isAtomic() || ld->isVolatile() ||
        !llvm::isa< llvm::GetElementPtrInst >(
            ld->getPointerOperand()->stripPointerCasts()) ||
        (ctx.opts.entry_points.size() == 1 &&
         ctx.opts.entry_points.front()->has_frontend() &&
         ctx.opts.entry_points.front()->frontend< llvm::Function >() ==
             ld->getFunction()))
      return false;
    ensure();
    return valid && certified.count(ld) && original(ld) == stmt;
  }
  bool excludes(const AccessRecord& a, const AccessRecord& b) {
    auto one = [&](const AccessRecord& h, const AccessRecord& g) {
      if (h.kind != AccessKind::Read || !h.call_context ||
          !h.call_context->empty() || !certifies(h.stmt))
        return false;
      auto* i =
          llvm::cast< llvm::Instruction >(h.stmt->frontend< llvm::Value >());
      return h.thread_id == i->getFunction()->getName().str() &&
             h.width == certified_widths.at(i) && direct_global(g);
    };
    return one(a, b) || one(b, a);
  }
};
ReadonlyHeapReads::ReadonlyHeapReads(Context& c) : _impl(new Impl(c)) {}
ReadonlyHeapReads::~ReadonlyHeapReads() = default;
bool ReadonlyHeapReads::certifies(ar::Statement* stmt) {
  return _impl->certifies(stmt);
}
bool ReadonlyHeapReads::excludes(const AccessRecord& a, const AccessRecord& b) {
  return _impl->excludes(a, b);
}
JsonDict ReadonlyHeapReads::info() const {
  return {{"readonly_heap_reads_contract", "closed-immutable-heap-reads-v1"},
          {"scope",
           "conditional nullable immutable heap byte sources; not pointer "
           "validity or completion"}};
}
} // namespace analyzer
} // namespace ikos
