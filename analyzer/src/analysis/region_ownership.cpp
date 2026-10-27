// Copyright and license: see execution_engine/numerical.hpp.
#include <ikos/ar/semantic/pthread.hpp>
#include <ikos/analyzer/analysis/region_ownership.hpp>

#include <algorithm>
#include <deque>
#include <map>
#include <set>
#include <tuple>

#include <llvm/ADT/Triple.h>
#include <llvm/IR/Operator.h>
#include <ikos/analyzer/analysis/bmc/unroll.hpp>
#include <ikos/analyzer/analysis/context.hpp>
#include <ikos/analyzer/analysis/execution_engine/heap_pointer_initialization.hpp>
#include <ikos/analyzer/analysis/execution_engine/pthread_initialization.hpp>
#include <ikos/analyzer/analysis/execution_engine/readonly_pointer_array.hpp>
#include <ikos/analyzer/analysis/literal.hpp>
#include <ikos/analyzer/analysis/variable.hpp>
#include <ikos/analyzer/util/log.hpp>

namespace ikos {
namespace analyzer {
namespace {
using core::ZNumber;
using Key = std::pair< ar::Statement*, CallContext* >;

struct Index {
  ar::Value* variable = nullptr;
  ZNumber constant{0};
  bool operator==(const Index& other) const {
    return variable == other.variable && constant == other.constant;
  }
};

enum class Kind {
  Unknown, Null, Root, Mutex, Owned, Fresh, Local, Global,
  Head, SharedOld, FromHead
};
struct Pointer {
  Kind kind = Kind::Unknown;
  Index index;
  ZNumber offset{0};
  ar::Value* identity = nullptr;
  bool operator==(const Pointer& other) const {
    return kind == other.kind && index == other.index &&
           offset == other.offset && identity == other.identity;
  }
};

struct Lock {
  bool held = false;
  Index index;
  bool symbolic = false;
  std::uint64_t key = 0;
  core::ConcurrentGlobalEnv::SymbolicIndex expression;
  bool operator==(const Lock& other) const {
    return held == other.held &&
           (!held || (index == other.index && symbolic == other.symbolic &&
                      key == other.key && expression == other.expression));
  }
};

// Finite MUST maps. Ownership is conditional on an actual live-heap access,
// not a proof of non-nullness. Missing bindings mean unknown; joins retain only
// common facts. Fresh instances are never equated through an allocation site.
struct State {
  std::map< ar::Value*, Pointer > pointers;
  std::map< ar::Value*, Index > indices;
  Lock lock;
  bool concurrent = false;

  bool operator==(const State& other) const {
    return pointers == other.pointers && indices == other.indices &&
           lock == other.lock && concurrent == other.concurrent;
  }
  void join_with(const State& other) {
    for (auto it = pointers.begin(); it != pointers.end();)
      if (!other.pointers.count(it->first) ||
          !(it->second == other.pointers.at(it->first)))
        it = pointers.erase(it);
      else
        ++it;
    for (auto it = indices.begin(); it != indices.end();)
      if (!other.indices.count(it->first) ||
          !(it->second == other.indices.at(it->first)))
        it = indices.erase(it);
      else
        ++it;
    if (!(lock == other.lock))
      lock = Lock{};
    concurrent |= other.concurrent;
  }
  void forget(ar::Value* value) {
    pointers.erase(value);
    indices.erase(value);
    for (auto it = pointers.begin(); it != pointers.end();)
      if (it->second.index.variable == value)
        it = pointers.erase(it);
      else
        ++it;
    for (auto it = indices.begin(); it != indices.end();)
      if (it->second.variable == value)
        it = indices.erase(it);
      else
        ++it;
    if (lock.held && lock.index.variable == value)
      lock = Lock{};
  }
};

struct Address {
  MemoryLocation* base = nullptr;
  bool bounded = false;
  bool nonnull = false;
  ZNumber lower{0}, upper{0};
  void join_with(const Address& other) {
    if (base != other.base)
      base = nullptr;
    nonnull &= other.nonnull;
    bounded &= other.bounded;
    lower = std::min(lower, other.lower);
    upper = std::max(upper, other.upper);
  }
};
struct Snapshot {
  std::map< ar::Value*, Address > addresses;
  std::set< std::uint64_t > locks;
};

ar::Function* callee(ar::CallBase* call) {
  return bmc::exact_callee(call);
}

bool nondet_api(ar::CallBase* call) {
  auto* function = callee(call);
  auto* type = call->has_result()
                   ? dyn_cast< ar::IntegerType >(call->result()->type())
                   : nullptr;
  auto* source =
      call->has_frontend()
          ? llvm::dyn_cast< llvm::CallInst >(call->frontend< llvm::Value >())
          : nullptr;
  auto* target = source ? llvm::dyn_cast< llvm::Function >(
                              source->getCalledOperand()->stripPointerCasts())
                        : nullptr;
  const bool verifier = function && function->name() == "__VERIFIER_nondet_int";
  if (!function || !function->is_declaration() ||
      (function->intrinsic_id() != ar::Intrinsic::IkosNonDet && !verifier) ||
      !type || call->num_arguments() != 0 || !target ||
      !target->isDeclaration() || (target->isVarArg() && !verifier) ||
      target->arg_size() != 0 || source->arg_size() != 0 ||
      source->getCallingConv() != llvm::CallingConv::C ||
      target->getCallingConv() != llvm::CallingConv::C ||
      !source->getType()->isIntegerTy(type->bit_width()) ||
      !target->getReturnType()->isIntegerTy(type->bit_width()))
    return false;
  auto name = target->getName();
  if (verifier) {
    // A C zero-parameter old-style declaration is represented as variadic.
    return name == "__VERIFIER_nondet_int" && type->bit_width() == 32 &&
           type->is_signed();
  }
  return (type->bit_width() == 32 &&
          name == (type->is_signed() ? "__ikos_nondet_int"
                                     : "__ikos_nondet_uint")) ||
         (type->bit_width() == 64 &&
          name == (type->is_signed() ? "__ikos_nondet_int64"
                                     : "__ikos_nondet_uint64"));
}

bool abort_api(ar::CallBase* call) {
  auto* function = callee(call);
  auto* source =
      call->has_frontend()
          ? llvm::dyn_cast< llvm::CallInst >(call->frontend< llvm::Value >())
          : nullptr;
  auto* target = source ? llvm::dyn_cast< llvm::Function >(
                              source->getCalledOperand()->stripPointerCasts())
                        : nullptr;
  return function && function->is_declaration() &&
         function->intrinsic_id() == ar::Intrinsic::LibcAbort &&
         call->num_arguments() == 0 && target && target->isDeclaration() &&
         target->getName() == "abort" && !target->isVarArg() &&
         target->arg_size() == 0 && source->arg_size() == 0 &&
         target->getReturnType()->isVoidTy() && source->getType()->isVoidTy() &&
         source->getCallingConv() == llvm::CallingConv::C &&
         target->getCallingConv() == llvm::CallingConv::C;
}

bool malloc_api(ar::CallBase* call) {
  if (!bmc::malloc_call(call))
    return false;
  auto* source =
      llvm::dyn_cast< llvm::CallInst >(call->frontend< llvm::Value >());
  auto* target = source ? llvm::dyn_cast< llvm::Function >(
                              source->getCalledOperand()->stripPointerCasts())
                        : nullptr;
  return target && source->getCallingConv() == llvm::CallingConv::C &&
         target->getCallingConv() == llvm::CallingConv::C;
}

bool output_api(ar::CallBase* call) {
  if (!bmc::constant_output_call(call) || !call->has_frontend())
    return false;
  auto* source =
      llvm::dyn_cast< llvm::CallInst >(call->frontend< llvm::Value >());
  auto* target = source ? llvm::dyn_cast< llvm::Function >(
                              source->getCalledOperand()->stripPointerCasts())
                        : nullptr;
  if (!target || !target->isDeclaration() ||
      !target->getReturnType()->isIntegerTy(32) ||
      !source->getType()->isIntegerTy(32) || target->arg_size() != 1 ||
      source->arg_size() != call->num_arguments() ||
      !target->getFunctionType()->getParamType(0)->isPointerTy() ||
      source->getCallingConv() != llvm::CallingConv::C ||
      target->getCallingConv() != llvm::CallingConv::C)
    return false;
  return (target->getName() == "printf" && target->isVarArg()) ||
         (target->getName() == "puts" && !target->isVarArg() &&
          source->arg_size() == 1);
}

// AR intrinsics do not replace the identity and ABI of the source call.
bool mutex_api(ar::CallBase* call, const char* name, unsigned arguments) {
  auto* source =
      call->has_frontend()
          ? llvm::dyn_cast< llvm::CallBase >(call->frontend< llvm::Value >())
          : nullptr;
  auto* target = source ? llvm::dyn_cast< llvm::Function >(
                              source->getCalledOperand()->stripPointerCasts())
                        : nullptr;
  auto* exact = callee(call);
  if (!exact || !exact->is_declaration() || !target ||
      !target->isDeclaration() || target->getName() != name ||
      target->isVarArg() || target->arg_size() != arguments ||
      source->arg_size() != arguments || call->num_arguments() != arguments ||
      !target->getReturnType()->isIntegerTy(32) ||
      !source->getType()->isIntegerTy(32) ||
      source->getCallingConv() != llvm::CallingConv::C ||
      target->getCallingConv() != llvm::CallingConv::C)
    return false;
  for (unsigned i = 0; i < arguments; ++i)
    if (!target->getFunctionType()->getParamType(i)->isPointerTy() ||
        !source->getArgOperand(i)->getType()->isPointerTy() ||
        !call->argument(i)->type()->is_pointer())
      return false;
  return true;
}

bool create_api(ar::CallBase* call) {
  auto* source =
      call->has_frontend()
          ? llvm::dyn_cast< llvm::CallBase >(call->frontend< llvm::Value >())
          : nullptr;
  auto* target = source ? llvm::dyn_cast< llvm::Function >(
                              source->getCalledOperand()->stripPointerCasts())
                        : nullptr;
  auto* exact = callee(call);
  if (!exact || !exact->is_declaration() || !ar::is_pthread_create(exact) ||
      !target || !target->isDeclaration() ||
      target->getName() != "pthread_create" || target->isVarArg() ||
      target->arg_size() != 4 || source->arg_size() != 4 ||
      call->num_arguments() != 4 || !target->getReturnType()->isIntegerTy(32) ||
      !source->getType()->isIntegerTy(32) ||
      source->getCallingConv() != llvm::CallingConv::C ||
      target->getCallingConv() != llvm::CallingConv::C)
    return false;
  for (unsigned i = 0; i < 4; ++i)
    if (!target->getFunctionType()->getParamType(i)->isPointerTy() ||
        !source->getArgOperand(i)->getType()->isPointerTy() ||
        !call->argument(i)->type()->is_pointer())
      return false;
  auto* output = dyn_cast< ar::PointerType >(call->argument(0)->type());
  auto* type =
      output ? dyn_cast< ar::IntegerType >(output->pointee()) : nullptr;
  const auto abi = pthread_object_layout(call->bundle(), source->getModule());
  auto* native_output = llvm::cast< llvm::PointerType >(
      target->getFunctionType()->getParamType(0));
  auto* actual_output = llvm::cast< llvm::PointerType >(
      source->getArgOperand(0)->getType());
  return abi && type && type->bit_width() == abi.word * 8 &&
         native_output->getAddressSpace() == 0 &&
         actual_output->getAddressSpace() == 0 &&
         !native_output->isOpaque() && !actual_output->isOpaque() &&
         native_output->getPointerElementType()->isIntegerTy(abi.word * 8) &&
         actual_output->getPointerElementType()->isIntegerTy(abi.word * 8);
}

bool matching_owner_node(const GlobalArraySubobject& roots,
                         ar::StructType* object,
                         const PthreadObjectLayout& abi) {
  auto* pointer = llvm::dyn_cast< llvm::PointerType >(
      roots.source_type->getElementType());
  auto* native = pointer && !pointer->isOpaque()
                     ? llvm::dyn_cast< llvm::StructType >(
                           pointer->getPointerElementType())
                     : nullptr;
  if (!abi || !pointer || pointer->getAddressSpace() != 0 || !native ||
      native->isOpaque() || native->isPacked() ||
      native->getNumElements() != object->num_fields())
    return false;
  auto* source = roots.global->frontend< llvm::GlobalVariable >();
  const auto& layout = source->getParent()->getDataLayout();
  const auto& ar_layout = roots.global->bundle()->data_layout();
  if (ar_layout.alloc_size_in_bytes(roots.type->element_type()) != abi.word ||
      ar_layout.store_size_in_bytes(roots.type->element_type()) != abi.word ||
      ZNumber(layout.getTypeAllocSize(native).getFixedValue()) !=
          ar_layout.alloc_size_in_bytes(object))
    return false;
  const auto* fields = layout.getStructLayout(native);
  unsigned i = 0;
  for (const auto& field : object->layout()) {
    auto* type = native->getElementType(i);
    if (!type->isSized() ||
        field.offset != ZNumber(fields->getElementOffset(i++)) ||
        ar_layout.store_size_in_bytes(field.type) !=
            ZNumber(layout.getTypeStoreSize(type).getFixedValue()))
      return false;
    if (auto* link = dyn_cast< ar::PointerType >(field.type)) {
      auto* native_link = llvm::dyn_cast< llvm::PointerType >(type);
      if (!native_link || native_link->isOpaque() ||
          native_link->getAddressSpace() != 0 ||
          native_link->getPointerElementType() != native ||
          link->pointee() != object ||
          ar_layout.store_size_in_bytes(link) != abi.word)
        return false;
    } else if (auto* integer = dyn_cast< ar::IntegerType >(field.type)) {
      if (!type->isIntegerTy(integer->bit_width()))
        return false;
    } else if (auto* floating = dyn_cast< ar::FloatType >(field.type)) {
      if (!type->isFloatingPointTy() ||
          type->getPrimitiveSizeInBits().getFixedValue() != floating->bit_width())
        return false;
    } else
      return false;
  }
  return true;
}

bool cyclic_edge(ar::BasicBlock* from, ar::BasicBlock* to) {
  std::set< ar::BasicBlock* > visited;
  std::vector< ar::BasicBlock* > pending{to};
  while (!pending.empty()) {
    auto* block = pending.back();
    pending.pop_back();
    if (block == from)
      return true;
    if (!visited.insert(block).second)
      continue;
    if (visited.size() > 256)
      return true;
    pending.insert(pending.end(),
                   block->successor_begin(),
                   block->successor_end());
  }
  return false;
}
} // namespace

class RegionOwnership::Impl {
  Context& ctx;
  GlobalArraySubobject roots, mutexes;
  PthreadObjectLayout abi;
  ZNumber count{0}, extent{0}, next{0};
  std::map< Key, Snapshot > snapshots;
  std::map< Key, bool > certificates;
  std::set< Key > conditional;
  std::map< MemoryLocation*, bool > concurrent_families;
  std::map< ar::CallBase*, bool > constructors;
  std::set< ar::Function* > active;
  std::set< ar::Function* > workers;
  bool valid = true;
  bool direct_heads = false;
  bool worker_analysis = false;
  unsigned remaining = 32768;
  std::string reason;

  bool fail(const char* why) {
    if (valid)
      reason = why;
    valid = false;
    return false;
  }

  Pointer pointer(ar::Value* value, const State& state) const {
    auto found = state.pointers.find(value);
    if (found != state.pointers.end())
      return found->second;
    Pointer result;
    if (isa< ar::NullConstant >(value))
      result.kind = Kind::Null;
    else if (value == roots.global && !roots.nested())
      result.kind = Kind::Root;
    else if (value == mutexes.global && !mutexes.nested())
      result.kind = Kind::Mutex;
    else if (isa< ar::GlobalVariable >(value)) {
      result.kind = Kind::Global;
      result.identity = value;
    } else if (isa< ar::LocalVariable >(value)) {
      result.kind = Kind::Local;
      result.identity = value;
    }
    return result;
  }
  Index index(ar::Value* value, const State& state) const {
    if (auto* constant = dyn_cast< ar::IntegerConstant >(value))
      return Index{nullptr, constant->value().to_z_number()};
    auto found = state.indices.find(value);
    return found == state.indices.end() ? Index{value, ZNumber(0)}
                                        : found->second;
  }
  const Snapshot* snapshot(ar::Statement* stmt, CallContext* cc) const {
    auto it = snapshots.find({stmt, cc});
    return it == snapshots.end() ? nullptr : &it->second;
  }
  const Address* address(ar::Statement* stmt,
                         CallContext* cc,
                         ar::Value* value) const {
    auto* snap = snapshot(stmt, cc);
    if (!snap)
      return nullptr;
    auto it = snap->addresses.find(value);
    return it == snap->addresses.end() ? nullptr : &it->second;
  }
  bool within(ar::Statement* stmt,
              CallContext* cc,
              ar::Value* value,
              const GlobalArraySubobject& object,
              unsigned width) const {
    auto* fact = address(stmt, cc, value);
    auto* base =
        fact ? dyn_cast_or_null< GlobalMemoryLocation >(fact->base) : nullptr;
    return fact && fact->bounded && fact->nonnull && base &&
           base->global_var() == object.global && fact->lower >= object.begin &&
           fact->upper + width <= object.begin + object.size;
  }
  bool held_at(ar::Statement* stmt, CallContext* cc, const Lock& lock) const {
    auto* snap = snapshot(stmt, cc);
    if (!lock.held || !snap)
      return false;
    auto* location = ctx.mem_factory->get_global(mutexes.global);
    auto base = core::IndexableTraits< MemoryLocation* >::index(location);
    for (auto key : snap->locks) {
      if (!lock.symbolic && key == lock.key)
        return true;
      std::uint64_t actual_base = 0;
      core::ConcurrentGlobalEnv::SymbolicIndex actual;
      if (lock.symbolic &&
          ctx.concurrent_env.lock_symbolic_index(key, actual_base, actual) &&
          actual_base == base && actual == lock.expression)
        return true;
    }
    return false;
  }
  void certify(ar::Statement* stmt,
               CallContext* cc,
               const Pointer& p,
               const State& state) {
    auto* operand = isa< ar::Load >(stmt) ? cast< ar::Load >(stmt)->operand()
                                          : cast< ar::Store >(stmt)->pointer();
    auto* fact = address(stmt, cc, operand);
    bool protected_access = (p.kind == Kind::Owned || p.kind == Kind::Head ||
                             p.kind == Kind::FromHead) && state.lock.held &&
                            p.index == state.lock.index && fact &&
                            held_at(stmt, cc, state.lock);
    auto inserted = certificates.emplace(Key{stmt, cc}, protected_access);
    if (!inserted.second)
      inserted.first->second &= protected_access;
    if (p.kind == Kind::FromHead)
      conditional.insert({stmt, cc});
  }
  void adopt(State& state, const Pointer& fresh, const Index& owner,
             Kind kind = Kind::Owned) {
    for (auto& binding : state.pointers)
      if (binding.second.kind == Kind::Fresh &&
          binding.second.identity == fresh.identity) {
        binding.second.kind = kind;
        binding.second.index = owner;
        binding.second.identity = nullptr;
      }
  }

  Pointer leaf_pointer(ar::Value* value, const State& state,
                       const GlobalArraySubobject& object, Kind kind) const {
    auto p = pointer(value, state);
    const ZNumber stride(kind == Kind::Root ? abi.word : abi.mutex);
    if (p.kind == Kind::Global && p.identity == object.global &&
        p.offset >= object.begin && p.offset < object.begin + object.size &&
        (p.offset - object.begin) % stride == 0) {
      p.kind = kind;
      p.index = Index{nullptr, (p.offset - object.begin) / stride};
      p.offset = 0;
      p.identity = nullptr;
    }
    return p;
  }

  bool shift(ar::PointerShift* statement, State& state) {
    auto p = pointer(statement->pointer(), state);
    ZNumber constant{0};
    ar::Value* variable = nullptr;
    ZNumber coefficient{0};
    for (auto it = statement->term_begin(); it != statement->term_end(); ++it) {
      auto c = (*it).first.to_z_number();
      auto* v = (*it).second;
      if (auto* n = dyn_cast< ar::IntegerConstant >(v))
        constant += c * n->value().to_z_number();
      else if (c != 0) {
        if (variable)
          return fail("multiple symbolic address terms");
        variable = v;
        coefficient = c;
      }
    }
    if (p.kind == Kind::Global && variable) {
      auto select = [&](const GlobalArraySubobject& object, Kind kind, unsigned stride) {
        const auto absolute = p.offset + constant;
        if (!object || p.identity != object.global || coefficient != stride ||
            absolute < object.begin || absolute >= object.begin + object.size ||
            (absolute - object.begin) % stride != 0) return false;
        p.kind = kind;
        p.index.constant = (absolute - object.begin) / stride;
        p.offset = 0;
        p.identity = nullptr;
        constant = 0;
        return true;
      };
      if (!select(roots, Kind::Root, abi.word) &&
          !select(mutexes, Kind::Mutex, abi.mutex))
        return fail("unsupported subarray selection");
    }
    if (p.kind == Kind::Root || p.kind == Kind::Mutex) {
      ZNumber stride(p.kind == Kind::Root ? abi.word : abi.mutex);
      if (p.index.variable || (variable && coefficient != stride) ||
          constant % stride != 0)
        return fail("non-leaf array address");
      auto idx = variable ? index(variable, state) : Index{};
      idx.constant += p.index.constant + constant / stride;
      p.index = idx;
    } else {
      if (variable)
        return fail("symbolic object-field offset");
      p.offset += constant;
      if ((p.kind == Kind::Owned || p.kind == Kind::Fresh ||
           p.kind == Kind::Head || p.kind == Kind::SharedOld ||
           p.kind == Kind::FromHead) &&
          (p.offset < 0 || p.offset > extent))
        return fail("object-field offset outside extent");
    }
    state.forget(statement->result());
    state.pointers[statement->result()] = p;
    return true;
  }

  bool transfer(ar::Statement* stmt, CallContext* cc, State& state);
  bool function(ar::Function* function,
                CallContext* cc,
                State& state,
                Pointer& returned);

public:
  explicit Impl(Context& context) : ctx(context) {
    for (auto it = ctx.bundle->global_begin(); it != ctx.bundle->global_end();
         ++it) {
      for (const auto& descriptor : global_array_subobjects(*it)) {
      auto* array = descriptor.type;
      auto* element =
          array ? dyn_cast< ar::PointerType >(array->element_type()) : nullptr;
      auto* object =
          element ? dyn_cast< ar::StructType >(element->pointee()) : nullptr;
      if (!array || array->num_elements() <= 0 || !object)
        continue;
      auto* source = descriptor.global->frontend< llvm::GlobalVariable >();
      const auto candidate_abi =
          pthread_object_layout(ctx.bundle, source->getParent());
      if (!matching_owner_node(descriptor, object, candidate_abi))
        continue;
      ZNumber field{0};
      unsigned links = 0;
      bool supported = true;
      for (const auto& f : object->layout()) {
        if (auto* pointer = dyn_cast< ar::PointerType >(f.type)) {
          supported &= pointer->pointee() == object;
          field = f.offset;
          ++links;
        } else
          supported &= f.type->is_integer() || f.type->is_float();
      }
      if (!supported || links != 1)
        continue;
      // The initial subset handles one family; ambiguity keeps the old may
      // analysis.
      if (roots) {
        roots = {};
        return;
      }
      roots = descriptor;
      abi = candidate_abi;
      count = array->num_elements();
      next = field;
      extent = ctx.bundle->data_layout().alloc_size_in_bytes(object);
      }
    }
  }

  void observe(ar::Statement* stmt,
               const value::AbstractDomain& inv,
               CallContext* cc) {
    if (!roots)
      return;
    Snapshot incoming;
    if (!inv.second().lockset_is_top())
      for (auto key : inv.second().held_mutexes())
        incoming.locks.insert(key);
    for (auto it = stmt->op_begin(); it != stmt->op_end(); ++it) {
      auto* operand = *it;
      if (!operand->type()->is_pointer() ||
          isa< ar::FunctionPointerConstant >(operand))
        continue;
      const auto& literal = ctx.lit_factory->get_scalar(operand);
      Address a;
      if (literal.is_pointer_var()) {
        auto ptr = inv.first().normal().pointer_to_pointer(literal.var());
        if (ptr.points_to().is_set() && ptr.points_to().size() == 1)
          a.base = *ptr.points_to().begin();
        a.nonnull = inv.first().normal().nullity_is_non_null(literal.var());
        if (!ptr.offset().is_bottom() && !ptr.offset().is_top()) {
          a.bounded = true;
          a.lower = ptr.offset().lb().to_z_number();
          a.upper = ptr.offset().ub().to_z_number();
        }
      }
      incoming.addresses.emplace(operand, std::move(a));
    }
    auto inserted = snapshots.emplace(Key{stmt, cc}, incoming);
    if (!inserted.second) {
      auto& old = inserted.first->second;
      for (auto& a : old.addresses) {
        auto found = incoming.addresses.find(a.first);
        if (found == incoming.addresses.end())
          a.second = Address{};
        else
          a.second.join_with(found->second);
      }
      std::set< std::uint64_t > common;
      std::set_intersection(old.locks.begin(),
                            old.locks.end(),
                            incoming.locks.begin(),
                            incoming.locks.end(),
                            std::inserter(common, common.end()));
      old.locks = std::move(common);
    }
  }

  void run() {
    if (!roots)
      return;
    if (ctx.opts.entry_points.size() != 1 ||
        ctx.opts.entry_points.front()->name() != "main" ||
        !ctx.opts.no_init_globals.empty() || ctx.concurrent_env.is_dirty() ||
        !ctx.concurrent_env.unmodeled_effects().empty() ||
        !ctx.concurrent_env.unmodeled_thread_creations().empty() ||
        ctx.bundle->global_or_null("ar.global_ctors") ||
        ctx.bundle->global_or_null("ar.global_dtors") ||
        !prove_readonly_pointer_array(roots, ctx)) {
      fail("root/publication coverage");
      return;
    }
    for (auto it = ctx.bundle->global_begin(); it != ctx.bundle->global_end();
         ++it) {
      for (const auto& descriptor : global_array_subobjects(*it)) {
        if (descriptor == roots) continue;
        if (mutex_array_storage_scope(descriptor, count)) {
          if (mutexes) {
            fail("ambiguous mutex family");
            return;
          }
          mutexes = descriptor;
        }
      }
    }
    if (!mutexes) {
      fail("mutex storage scope");
      return;
    }
    // Close all calls, including code not selected by the original AI. Unknown
    // libraries could touch globals without receiving their address explicitly.
    for (auto fit = ctx.bundle->function_begin();
         fit != ctx.bundle->function_end();
         ++fit) {
      if (!(*fit)->is_definition())
        continue;
      for (auto* block : *(*fit)->body())
        for (auto* stmt : *block) {
          auto* call = dyn_cast< ar::CallBase >(stmt);
          if (!call)
            continue;
          auto* target = callee(call);
          if (!target) {
            fail("indirect call effects");
            return;
          }
          if (target->is_definition() || nondet_api(call) || abort_api(call) ||
              malloc_api(call) || output_api(call) ||
              mutex_api(call, "pthread_mutex_init", 2) ||
              mutex_api(call, "pthread_mutex_lock", 1) ||
              mutex_api(call, "pthread_mutex_unlock", 1))
            continue;
          if (create_api(call) && call->code()->function()->name() == "main" &&
              isa< ar::NullConstant >(call->argument(1)) &&
              isa< ar::NullConstant >(call->argument(3))) {
            auto* child =
                dyn_cast< ar::FunctionPointerConstant >(call->argument(2));
            if (!child || !child->function()->is_definition() ||
                child->function()->name() == "main") {
              fail("thread entry scope");
              return;
            }
            workers.insert(child->function());
            continue;
          }
          fail("uncovered library effects");
          return;
        }
    }
    if (workers.empty()) {
      fail("no closed workers");
      return;
    }
    prove();
    if (!valid) {
      // Recheck the complete frame with a weaker graph invariant. No facts
      // from the failed unique-region proof may enter the direct-head proof.
      valid = true;
      direct_heads = true;
      reason.clear();
      remaining = 32768;
      active.clear();
      certificates.clear();
      conditional.clear();
      concurrent_families.clear();
      prove();
    }
  }
  void prove() {
    worker_analysis = false;
    auto* cc = ctx.call_context_factory->get_empty();
    State state;
    Pointer ignored;
    if (!function(ctx.opts.entry_points.front(), cc, state, ignored))
      return;
    std::vector< ar::Function* > ordered_workers(workers.begin(),
                                                 workers.end());
    std::sort(ordered_workers.begin(),
              ordered_workers.end(),
              [](ar::Function* a, ar::Function* b) {
                return a->name() < b->name();
              });
    for (auto* worker : ordered_workers) {
      worker_analysis = true;
      State entry;
      entry.concurrent = true;
      if (worker->num_parameters() != 1 ||
          !(*worker->param_begin())->type()->is_pointer() ||
          !worker->type()->return_type()->is_pointer()) {
        fail("thread argument ABI");
        return;
      }
      entry.pointers[*worker->param_begin()].kind = Kind::Null;
      if (!function(worker, cc, entry, ignored))
        return;
    }
    log::debug(
        "Region ownership: complete family and all writer transfers certified");
  }
  bool excludes(ar::Statement* a,
                CallContext* ac,
                ar::Statement* b,
                CallContext* bc,
                MemoryLocation* common) const {
    auto x = certificates.find({a, ac}), y = certificates.find({b, bc});
    if (!valid || !roots || !mutexes || x == certificates.end() ||
        y == certificates.end() || !x->second || !y->second)
      return false;
    if (conditional.count({a, ac}) || conditional.count({b, bc})) {
      auto family = concurrent_families.find(common);
      return family != concurrent_families.end() && family->second;
    }
    return true;
  }
  void diagnose() const {
    if (!log::is_enabled_for(LogLevel::Debug)) return;
    const char* status = !roots ? "not-applicable" :
        valid ? "proved" :
        reason == "finite owner analysis budget" ? "internal-bound" : "missing-premise";
    log::debug(std::string("Cooperation owner: status=") + status +
        " scope=family-transfer-completion mode=" +
        (direct_heads ? "direct-heads" : "indexed-live-heap") +
        " reason=" + reason +
        " snapshots=" + std::to_string(snapshots.size()) +
        " certificate_records=" + std::to_string(certificates.size()) +
        " remaining=" + std::to_string(
            reason == "finite owner analysis budget" ? 0 : remaining));
  }
  JsonDict info() const {
    return {{"ownership_contract", direct_heads
                                      ? "indexed-head-publication-owner-v1"
                                      : "indexed-live-heap-owner-v1"},
            {"scope", "paired live-heap byte conflicts, not pointer validity"},
            {"root_array", roots ? roots.global->name() : ""},
            {"mutex_array", mutexes ? mutexes.global->name() : ""},
            {"root_byte_begin", roots.begin.str()},
            {"mutex_byte_begin", mutexes.begin.str()},
            {"array_count", count.str()},
             {"pointer_bytes", abi.word},
             {"mutex_bytes", abi.mutex},
            {"object_extent", extent.str()},
            {"pointer_field_offset", next.str()}};
  }
  ~Impl() {
    if (roots && !valid)
      log::debug("Region ownership unsupported: " + reason);
  }
};

bool RegionOwnership::Impl::transfer(ar::Statement* stmt,
                                     CallContext* cc,
                                     State& state) {
  if (auto* copy = dyn_cast< ar::Assignment >(stmt)) {
    auto p = pointer(copy->operand(), state);
    auto i = index(copy->operand(), state);
    state.forget(copy->result());
    if (copy->result()->type()->is_pointer())
      state.pointers[copy->result()] = p;
    // Keep a mutable index's own value identity even on its first constant
    // iteration, so joins preserve the corresponding-lock relation.
    else if (!isa< ar::IntegerConstant >(copy->operand()) &&
             copy->result()->type() == copy->operand()->type() &&
             i.variable != copy->result())
      state.indices[copy->result()] = i;
    return true;
  }
  if (auto* cast = dyn_cast< ar::UnaryOperation >(stmt)) {
    auto p = pointer(cast->operand(), state);
    auto i = index(cast->operand(), state);
    auto* from = dyn_cast< ar::IntegerType >(cast->operand()->type());
    auto* to = dyn_cast< ar::IntegerType >(cast->result()->type());
    const bool same_integer = from && to && to->bit_width() > from->bit_width() &&
        ((cast->op() == ar::UnaryOperation::SExt && from->is_signed() &&
          to->is_signed()) ||
         (cast->op() == ar::UnaryOperation::ZExt && !from->is_signed()));
    bool pointer_cast = cast->op() == ar::UnaryOperation::Bitcast &&
                        cast->operand()->type()->is_pointer() &&
                        cast->result()->type()->is_pointer();
    if (cast->operand()->type()->is_pointer() !=
        cast->result()->type()->is_pointer())
      return fail("pointer/integer representation conversion");
    state.forget(cast->result());
    if (pointer_cast)
      state.pointers[cast->result()] = p;
    else if (same_integer && i.variable != cast->result())
      state.indices[cast->result()] = i;
    return true;
  }
  if (auto* shift_stmt = dyn_cast< ar::PointerShift >(stmt))
    return shift(shift_stmt, state);
  if (auto* operation = dyn_cast< ar::BinaryOperation >(stmt)) {
    if (!operation->result()->type()->is_integer() &&
        !operation->result()->type()->is_float())
      return fail("non-scalar arithmetic");
    state.forget(operation->result());
    return true;
  }
  if (isa< ar::Comparison >(stmt))
    return true;
  if (auto* allocation = dyn_cast< ar::Allocate >(stmt)) {
    Pointer p;
    p.kind = Kind::Local;
    p.identity = allocation->result();
    state.forget(allocation->result());
    state.pointers[allocation->result()] = p;
    return true;
  }
  if (isa< ar::Load >(stmt) || isa< ar::Store >(stmt)) {
    auto* load = dyn_cast< ar::Load >(stmt);
    auto* store = dyn_cast< ar::Store >(stmt);
    auto* operand = load ? load->operand() : store->pointer();
    auto* type = load ? load->result()->type() : store->value()->type();
    auto width = ctx.bundle->data_layout().store_size_in_bytes(type);
    auto p = leaf_pointer(operand, state, roots, Kind::Root);
    if ((load && (load->is_atomic() || load->is_volatile())) ||
        (store && (store->is_atomic() || store->is_volatile())))
      return fail("atomic/volatile graph access");
    Pointer value;
    if (p.kind == Kind::Root) {
      if (!pointer_array_access(stmt, roots) ||
          !within(stmt, cc, operand, roots, abi.word))
        return fail("root leaf range");
      if (load) {
        value.kind = direct_heads ? Kind::Head : Kind::Owned;
        value.index = p.index;
      } else {
        if (state.concurrent || !type->is_pointer())
          return fail("root publication after create");
        auto rhs = pointer(store->value(), state);
        if (rhs.kind == Kind::Fresh && rhs.offset == 0)
          adopt(state, rhs, p.index, direct_heads ? Kind::Head : Kind::Owned);
        else if (rhs.kind != Kind::Null)
          return fail("non-fresh root publication");
      }
    } else if (p.kind == Kind::Owned || p.kind == Kind::Fresh ||
               p.kind == Kind::Head || p.kind == Kind::SharedOld ||
               p.kind == Kind::FromHead) {
      if (p.offset < 0 || p.offset + width > extent || width <= 0)
        return fail("incomplete object access extent");
      if (type->is_pointer()) {
        if (p.offset != next || width != abi.word)
          return fail("uncovered pointer field layout");
        if (direct_heads) {
          if (load) {
            if (p.kind != Kind::Head && p.kind != Kind::Owned)
              return fail("unproved transitive head traversal");
            value.kind = Kind::FromHead;
            value.index = p.index;
          } else {
            auto rhs = pointer(store->value(), state);
            if (p.kind == Kind::FromHead || rhs.offset != 0)
              return fail("indirect graph publication");
            if (state.concurrent) {
              if ((p.kind != Kind::Head && p.kind != Kind::Owned) ||
                  (rhs.kind != Kind::Null &&
                   !((rhs.kind == Kind::Owned || rhs.kind == Kind::FromHead) &&
                     rhs.index == p.index)))
                return fail("foreign concurrent head publication");
            } else {
              if (rhs.kind == Kind::Fresh)
                adopt(state, rhs, Index{}, Kind::SharedOld);
              else if (rhs.kind != Kind::Null && rhs.kind != Kind::SharedOld &&
                       rhs.kind != Kind::FromHead)
                return fail("head or unknown node stored in old graph");
            }
          }
        } else if (load) {
          if (p.kind != Kind::Owned)
            return fail("unbound fresh field load");
          value.kind = Kind::Owned;
          value.index = p.index;
        } else {
          auto rhs = pointer(store->value(), state);
          if (rhs.kind == Kind::Fresh && p.kind == Kind::Owned &&
              rhs.offset == 0) {
            adopt(state, rhs, p.index);
          } else if (rhs.kind == Kind::Owned && p.kind == Kind::Owned &&
                     rhs.offset == 0 && rhs.index == p.index) {
            // The shared next-field schema is preserved.
          } else if (rhs.kind != Kind::Null)
            return fail("cross-owner or fresh graph edge");
        }
      } else {
        if (!type->is_integer() && !type->is_float())
          return fail("aggregate object access");
        if (p.offset < next + abi.word && next < p.offset + width)
          return fail("scalar write/read overlaps pointer representation");
      }
      certify(stmt, cc, p, state);
    } else if ((p.kind == Kind::Local || p.kind == Kind::Global) &&
               (type->is_integer() || type->is_float())) {
      // These disjoint scalar cells cannot publish or modify the owner graph.
    } else
      return fail("unknown or escaped pointer access");
    if (load) {
      state.forget(load->result());
      if (type->is_pointer())
        state.pointers[load->result()] = value;
    }
    return true;
  }
  if (auto* call = dyn_cast< ar::CallBase >(stmt)) {
    auto* target = callee(call);
    if (!target)
      return fail("unknown call target");
    Pointer result;
    if (target->is_definition()) {
      if (target->num_parameters() != call->num_arguments())
        return fail("helper parameter ABI");
      auto* child = ctx.call_context_factory->get_context(cc, call);
      auto param = target->param_begin();
      for (unsigned n = 0; n < call->num_arguments(); ++n, ++param) {
        // Capture actuals before overwriting callee-local parameter names.
        if ((*param)->type()->is_pointer())
          state.pointers[*param] = pointer(call->argument(n), state);
        else
          state.indices[*param] = index(call->argument(n), state);
      }
      if (!function(target, child, state, result))
        return false;
    } else if (malloc_api(call)) {
      auto proof = constructors.emplace(call, false);
      if (proof.second)
        proof.first->second = prove_heap_constructor_body(call, next);
      auto* size = call->num_arguments() == 1
                       ? dyn_cast< ar::IntegerConstant >(call->argument(0))
                       : nullptr;
      if (!proof.first->second || !size ||
          size->value().to_z_number() != extent)
        return fail("allocation constructor coverage");
      for (const auto& binding : state.pointers)
        if (binding.second.kind == Kind::Fresh)
          return fail("multiple unbound fresh instances");
      if (state.concurrent && !state.lock.held)
        return fail("unprotected concurrent construction");
      if (direct_heads) {
        // MAY-spawned at a CFG join does not certify an allocation phase.
        // Only actual worker-root executions qualify, with every birth held.
        auto* family = ctx.mem_factory->get_dyn_alloc(call, cc);
        const bool eligible = worker_analysis && state.lock.held;
        auto inserted = concurrent_families.emplace(family, eligible);
        if (!inserted.second)
          inserted.first->second &= eligible;
      }
      if (state.lock.held && (!direct_heads || worker_analysis)) {
        result.kind = Kind::Owned;
        result.index = state.lock.index;
      } else {
        result.kind = Kind::Fresh;
        result.identity = call->result();
      }
    } else if (mutex_api(call, "pthread_mutex_init", 2)) {
      auto p = leaf_pointer(call->argument(0), state, mutexes, Kind::Mutex);
      if (state.concurrent || state.lock.held || p.kind != Kind::Mutex ||
          !isa< ar::NullConstant >(call->argument(1)) ||
          !within(call, cc, call->argument(0), mutexes, abi.mutex))
        return fail("mutex initialization scope");
    } else if (mutex_api(call, "pthread_mutex_lock", 1)) {
      auto p = leaf_pointer(call->argument(0), state, mutexes, Kind::Mutex);
      auto* fact = address(call, cc, call->argument(0));
      if (p.kind != Kind::Mutex || state.lock.held ||
          !within(call, cc, call->argument(0), mutexes, abi.mutex))
        return fail("mutex acquisition scope/range");
      state.lock.held = true;
      state.lock.index = p.index;
      auto base = core::IndexableTraits< MemoryLocation* >::index(
          ctx.mem_factory->get_global(mutexes.global));
      if (fact->lower == fact->upper)
        state.lock.key = offset_lock_key(base, fact->lower);
      else {
        state.lock.symbolic = true;
        if (!symbolic_index_of(call->argument(0), state.lock.expression, cc))
          return fail("missing actual mutex expression");
      }
    } else if (mutex_api(call, "pthread_mutex_unlock", 1)) {
      auto p = leaf_pointer(call->argument(0), state, mutexes, Kind::Mutex);
      if (p.kind != Kind::Mutex || !state.lock.held ||
          !(p.index == state.lock.index) ||
          !within(call, cc, call->argument(0), mutexes, abi.mutex))
        return fail("mutex release scope/range");
      state.lock = Lock{};
    } else if (create_api(call)) {
      auto output = pointer(call->argument(0), state);
      auto* local = dyn_cast_or_null< ar::LocalVariable >(output.identity);
      auto* handle = local
                         ? dyn_cast< ar::IntegerType >(local->type()->pointee())
                         : nullptr;
      auto* fact = address(call, cc, call->argument(0));
      if (state.lock.held || output.kind != Kind::Local || output.offset != 0 ||
          !local || !handle || handle->bit_width() != abi.word * 8 ||
          ctx.bundle->data_layout().alloc_size_in_bytes(
              local->type()->pointee()) != abi.word ||
          !fact || !fact->nonnull || !fact->bounded || fact->lower != 0 ||
          fact->upper != 0 || fact->base != ctx.mem_factory->get_local(local))
        return fail("create output scope");
      state.concurrent = true;
    } else if (!nondet_api(call) && !output_api(call)) {
      return fail("uncovered call transfer");
    }
    if (call->has_result()) {
      state.forget(call->result());
      if (call->result()->type()->is_pointer())
        state.pointers[call->result()] = result;
    }
    return true;
  }
  return fail("uncovered AR transfer");
}

bool RegionOwnership::Impl::function(ar::Function* fun,
                                     CallContext* cc,
                                     State& state,
                                     Pointer& returned) {
  if (!valid || !fun->is_definition() || !active.insert(fun).second)
    return fail("recursive or missing helper body");
  auto* body = fun->body();
  std::map< ar::BasicBlock*, State > entries;
  std::deque< ar::BasicBlock* > pending;
  entries.emplace(body->entry_block(), state);
  pending.push_back(body->entry_block());
  bool has_return = false;
  State exit;
  Pointer result;
  while (!pending.empty() && valid) {
    auto* block = pending.front();
    pending.pop_front();
    State current = entries.at(block);
    bool terminal = false;
    for (auto* statement : *block) {
      if (remaining-- == 0)
        return fail("finite owner analysis budget");
      if (auto* ret = dyn_cast< ar::ReturnValue >(statement)) {
        Pointer value;
        if (ret->has_operand() && ret->operand()->type()->is_pointer())
          value = pointer(ret->operand(), current);
        if (!has_return) {
          exit = current;
          result = value;
          has_return = true;
        } else {
          exit.join_with(current);
          if (!(result == value))
            result = Pointer{};
        }
        terminal = true;
        break;
      }
      if (isa< ar::Unreachable >(statement)) {
        terminal = true;
        break;
      }
      if (auto* call = dyn_cast< ar::CallBase >(statement)) {
        if (abort_api(call)) {
          // Preserve preceding accesses, but there is no normal continuation.
          terminal = true;
          break;
        }
      }
      if (!transfer(statement, cc, current)) {
        if (log::is_enabled_for(LogLevel::Debug)) {
          auto message = log::msg();
          message << "Region ownership stopped at: ";
          statement->dump(message.stream());
          message << "\n";
        }
        return false;
      }
    }
    if (terminal)
      continue;
    for (auto it = block->successor_begin(); it != block->successor_end();
         ++it) {
      if (cyclic_edge(block, *it)) {
        for (const auto& binding : current.pointers)
          if (binding.second.kind == Kind::Fresh)
            return fail("active fresh instance across cycle");
      }
      auto inserted = entries.emplace(*it, current);
      if (inserted.second)
        pending.push_back(*it);
      else {
        State merged = inserted.first->second;
        merged.join_with(current);
        if (!(merged == inserted.first->second)) {
          inserted.first->second = std::move(merged);
          pending.push_back(*it);
        }
      }
    }
  }
  active.erase(fun);
  if (!valid || !has_return)
    return fail("missing supported helper return");
  if (auto* variable =
          dyn_cast_or_null< ar::InternalVariable >(result.index.variable))
    if (variable->code() == body)
      result = Pointer{};
  // Local aliases do not outlive a helper activation. Returned owner indices
  // already refer to actual caller values, not to reusable formal identities.
  for (auto it = fun->param_begin(); it != fun->param_end(); ++it)
    exit.forget(*it);
  for (auto* block : *body)
    for (auto* statement : *block)
      if (auto* value = statement->result_or_null())
        exit.forget(value);
  state = std::move(exit);
  returned = result;
  return true;
}

RegionOwnership::RegionOwnership(Context& ctx) : _impl(new Impl(ctx)) {}
RegionOwnership::~RegionOwnership() = default;
void RegionOwnership::observe(ar::Statement* stmt,
                              const value::AbstractDomain& inv,
                              CallContext* cc) {
  _impl->observe(stmt, inv, cc);
}
void RegionOwnership::run() {
  _impl->run();
  _impl->diagnose();
}
bool RegionOwnership::excludes(ar::Statement* a,
                               CallContext* ac,
                               ar::Statement* b,
                               CallContext* bc,
                               MemoryLocation* common) const {
  return _impl->excludes(a, ac, b, bc, common);
}
JsonDict RegionOwnership::info() const {
  return _impl->info();
}
} // namespace analyzer
} // namespace ikos
