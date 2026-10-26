// Copyright and license: see data_race.hpp.
#include <ikos/analyzer/checker/array_slices.hpp>

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
enum class Place { Global, Local, Heap, Null };
struct Pointer {
  Place place;
  llvm::Value* object = nullptr;
  llvm::GlobalVariable* root = nullptr;
  llvm::Value* index = nullptr;
  std::uint64_t stride = 0;
  z3::expr offset, extent;
  Pointer(z3::context& z, Place p)
      : place(p), offset(z.bv_val(0, 64)), extent(offset) {}
};
struct State {
  std::map< llvm::Value*, z3::expr > values, cells;
  std::map< llvm::Value*, Pointer > pointers;
  z3::expr path;
  bool spawned = false;
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
struct Range {
  llvm::GlobalVariable* root;
  unsigned world;
  std::uint64_t stride;
  z3::expr low, high;
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

class ArraySlices::Impl {
  Context& ctx;
  z3::context z;
  llvm::Function* main = nullptr;
  std::map< llvm::Value*, ar::Statement* > originals;
  std::map< llvm::Function*, ar::Function* > functions;
  std::set< llvm::Function* > active, workers;
  std::vector< State > worlds;
  std::map< llvm::Instruction*, std::vector< Range > > ranges;
  std::set< llvm::Instruction* > incomplete_ranges;
  unsigned budget = 32768, queries = 512, symbols = 0, world = 0;
  bool attempted = false, valid = false;
  llvm::Instruction* last = nullptr;

  void spend() {
    if (!budget--)
      throw Refuse{};
  }
  z3::expr fresh(unsigned bits) {
    return z.bv_const(("slice_" + std::to_string(++symbols)).c_str(), bits);
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
    if ((name == "__VERIFIER_nondet_int" || name == "__VERIFIER_nondet_uint") &&
        bits == 32 && t->has_frontend())
      return true;
    if (name == "__VERIFIER_nondet_ulonglong" && bits == 64 &&
        t->has_frontend())
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
      if (c->getOperand(0)->getType()->isPointerTy()) {
        auto a = pointer(s, c->getOperand(0)), b = pointer(s, c->getOperand(1));
        if (!c->isEquality() ||
            !((a.place == Place::Heap && b.place == Place::Null) ||
              (a.place == Place::Null && b.place == Place::Heap)))
          throw Refuse{};
        // Both outcomes remain possible; neither branch refines the pointer.
        return fresh(1);
      }
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
      const unsigned width = a.get_sort().bv_size();
      if (b->hasNoSignedWrap() || b->hasNoUnsignedWrap()) {
        if (width > 32)
          throw Refuse{};
        for (bool sign : {false, true}) {
          if (!(sign ? b->hasNoSignedWrap() : b->hasNoUnsignedWrap()))
            continue;
          auto x = sign ? z3::sext(a, width) : z3::zext(a, width);
          auto y = sign ? z3::sext(c, width) : z3::zext(c, width);
          auto wide = b->getOpcode() == llvm::Instruction::Add   ? x + y
                      : b->getOpcode() == llvm::Instruction::Sub ? x - y
                                                                 : x * y;
          require(s, wide == (sign ? z3::sext(r, width) : z3::zext(r, width)));
        }
      }
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
    if (mode == Mode::Constructor || mode == Mode::Worker) {
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
              p->getNumIncomingValues() != 2)
            throw Refuse{};
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
      if (!phi || !compare ||
          compare->getPredicate() != llvm::CmpInst::ICMP_SLT ||
          compare->getOperand(0) != phi ||
          !loop->contains(branch->getSuccessor(0)) ||
          loop->contains(branch->getSuccessor(1)) || !step ||
          step->getOpcode() != llvm::Instruction::Add ||
          step->getOperand(0) != phi || !one || !one->isOne() ||
          !dom.dominates(branch->getSuccessor(0), latch))
        throw Refuse{};
      upper = compare->getOperand(1);
      // A changing heap value or nondet loop bound is not a stable interval
      // end.
      std::set< llvm::Value* > seen;
      std::function< bool(llvm::Value*) > stable = [&](llvm::Value* v) {
        if (!seen.insert(v).second)
          return false;
        if (llvm::isa< llvm::ConstantInt >(v) || llvm::isa< llvm::Argument >(v))
          return true;
        if (auto* load = llvm::dyn_cast< llvm::LoadInst >(v))
          return llvm::isa< llvm::GlobalVariable >(
              load->getPointerOperand()->stripPointerCasts());
        if (auto* cast = llvm::dyn_cast< llvm::CastInst >(v))
          return stable(cast->getOperand(0));
        return false;
      };
      if (!stable(upper))
        throw Refuse{};
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
              !dom.dominates(allocation, r))
            throw Refuse{};
          ++returns;
        }
      if (!returns)
        throw Refuse{};
    }
    std::deque< Job > pending;
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
      if (loop && i == &loop->getHeader()->front() &&
          j.predecessor == loop->getLoopPreheader()) {
        j.low = number(s, phi->getIncomingValueForBlock(j.predecessor));
        j.iterator = fresh(32);
        set(s, phi, *j.iterator);
        s.path = s.path && *j.iterator >= *j.low;
      }
      if (auto* p = llvm::dyn_cast< llvm::PHINode >(i)) {
        if (p != phi) {
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
          if (loop && i->getParent() == latch && next == loop->getHeader()) {
            auto increment = number(s, phi->getIncomingValueForBlock(latch));
            require(child.state,
                    increment > *j.iterator && increment >= *j.low);
            continue;
          }
          child.pc = &next->front();
          child.predecessor = i->getParent();
          pending.push_back(std::move(child));
        }
        continue;
      } else if (auto* ret = llvm::dyn_cast< llvm::ReturnInst >(i)) {
        FunctionResult r{std::move(s), {}, {}};
        if (auto* v = ret->getReturnValue()) {
          if (scalar(v->getType()))
            r.number = number(r.state, v);
          else if (v->getType()->isPointerTy())
            r.pointer = pointer(r.state, v);
          else
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
            !scalar(gep->getSourceElementType()))
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
            conversion->getOperand(0)->getType()->isPointerTy())
          put(s.pointers, i, pointer(s, conversion->getOperand(0)));
        else
          set(s, i, operation(s, i));
      } else if (llvm::isa< llvm::LoadInst >(i) ||
                 llvm::isa< llvm::StoreInst >(i)) {
        bool recorded_range = false;
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
                    p.offset + z.bv_val(width, 64) <= p.extent);
        if (p.place == Place::Global) {
          auto* g = llvm::cast< llvm::GlobalVariable >(p.object);
          if (mode == Mode::Constructor ||
              (store && (mode != Mode::Main || s.spawned)) || !zero(p.offset) ||
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
            set(s, i, number(s, g));
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
          if (!scalar(type) || width > p.stride)
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
          if (load)
            set(s, i, fresh(type->getIntegerBitWidth()));
          else
            (void)number(s, store->getValueOperand());
          if (mode == Mode::Worker && p.root && p.index == phi && j.low &&
              j.iterator && loop && loop->contains(i) &&
              i->getParent() != loop->getHeader()) {
            auto* ar_ptr = load ? cast< ar::Load >(a)->operand()
                                : cast< ar::Store >(a)->pointer();
            if (!ar_index(ar_ptr, phi, p.stride))
              throw Refuse{};
            require(s, *j.iterator >= *j.low && *j.iterator < number(s, upper));
            require(s,
                    number(s, phi) >= z.bv_val(0, 32) &&
                        extend(number(s, phi)) * z.bv_val(p.stride, 64) +
                                z.bv_val(width, 64) <=
                            z.bv_val(UINT64_C(0xffffffff), 64));
            ranges[i].push_back(
                {p.root, world, p.stride, *j.low, number(s, upper)});
            recorded_range = true;
          }
        } else
          throw Refuse{};
        // A range on another path cannot certify this occurrence.
        if (mode == Mode::Worker && !recorded_range)
          incomplete_ranges.insert(i);
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
          if (mode != Mode::Constructor || call != allocation ||
              call->arg_size() != 1 || called->getName() != "malloc" ||
              !call->getType()->isPointerTy() ||
              !call->getArgOperand(0)->getType()->isIntegerTy(32))
            throw Refuse{};
          auto length = number(s, f->getArg(0)),
               size = number(s, call->getArgOperand(0));
          require(s,
                  length >= z.bv_val(0, 32) &&
                      extend(size) == extend(length) * z.bv_val(stride, 64) &&
                      extend(length) * z.bv_val(stride, 64) <=
                          z.bv_val(UINT64_C(0xffffffff), 64));
          Pointer p(z, Place::Heap);
          p.object = call;
          p.stride = stride;
          p.extent = extend(size);
          put(s.pointers, i, p);
        } else if (native_handle_api(call, true)) {
          if (mode != Mode::Main ||
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
            worlds.push_back(s);
            s.spawned = true;
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
        !ctx.opts.no_init_globals.empty() || ctx.concurrent_env.is_dirty() ||
        !ctx.concurrent_env.unmodeled_effects().empty() ||
        !ctx.concurrent_env.unmodeled_thread_creations().empty() ||
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
  llvm::Instruction* endpoint(const AccessRecord& r) const {
    if (!r.stmt->has_frontend() || !r.call_context->empty() ||
        !r.pts.is_set() || r.pts.is_empty() || !r.width)
      return nullptr;
    for (auto* object : r.pts)
      if (!isa< DynAllocMemoryLocation >(object))
        return nullptr;
    auto* i =
        llvm::dyn_cast< llvm::Instruction >(r.stmt->frontend< llvm::Value >());
    if (!i || r.thread_id != i->getFunction()->getName().str() ||
        (!llvm::isa< llvm::LoadInst >(i) && !llvm::isa< llvm::StoreInst >(i)))
      return nullptr;
    return i;
  }

public:
  explicit Impl(Context& c) : ctx(c) {}
  bool excludes(const AccessRecord& a, const AccessRecord& b) {
    auto* x = endpoint(a);
    auto* y = endpoint(b);
    if (!x || !y || x == y)
      return false;
    // Avoid symbolic work for scalar globals and ordinary pointer accesses.
    auto indexed = [](llvm::Instruction* i) {
      auto* p = llvm::isa< llvm::LoadInst >(i)
                    ? llvm::cast< llvm::LoadInst >(i)->getPointerOperand()
                    : llvm::cast< llvm::StoreInst >(i)->getPointerOperand();
      return llvm::isa< llvm::GetElementPtrInst >(p->stripPointerCasts());
    };
    if (!indexed(x) || !indexed(y))
      return false;
    if (!attempted) {
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
        ranges.clear();
        if (log::is_enabled_for(LogLevel::Debug)) {
          std::string text;
          llvm::raw_string_ostream stream(text);
          if (last)
            last->print(stream);
          log::debug("Array slices: incomplete frame or induction at " +
                     stream.str());
        }
      }
    }
    if (!valid || original(x) != a.stmt || original(y) != b.stmt ||
        !ranges.count(x) || !ranges.count(y) ||
        incomplete_ranges.count(x) || incomplete_ranges.count(y))
      return false;
    try {
      for (unsigned w = 0; w < worlds.size(); ++w) {
        bool covered = false;
        for (const auto& p : ranges.at(x))
          for (const auto& q : ranges.at(y)) {
            if (p.world != w || q.world != w)
              continue;
            covered = true;
            if (p.root != q.root || p.stride != q.stride ||
                a.width > p.stride || b.width > q.stride)
              return false;
            require(worlds[w],
                    extend(p.high, true) <= extend(q.low, true) ||
                        extend(q.high, true) <= extend(p.low, true));
          }
        if (!covered)
          return false;
      }
      return true;
    } catch (const Refuse&) {
      valid = false;
      ranges.clear();
      return false;
    } catch (const z3::exception&) {
      valid = false;
      ranges.clear();
      return false;
    }
  }
};
ArraySlices::ArraySlices(Context& c) : _impl(new Impl(c)) {}
ArraySlices::~ArraySlices() = default;
bool ArraySlices::excludes(const AccessRecord& a, const AccessRecord& b) {
  return _impl->excludes(a, b);
}
JsonDict ArraySlices::info() const {
  return {{"array_slices_contract", "closed-stable-flat-array-slices-v1"},
          {"scope",
           "inductive complete byte ranges; not pointer validity or "
           "completion"}};
}
} // namespace analyzer
} // namespace ikos
