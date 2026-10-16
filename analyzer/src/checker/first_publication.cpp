// Copyright and license: see data_race.hpp.
#include <ikos/analyzer/checker/first_publication.hpp>

#include <llvm/IR/Operator.h>
#include <algorithm>
#include <deque>
#include <ikos/analyzer/analysis/bmc/unroll.hpp>
#include <ikos/analyzer/analysis/execution_engine/heap_initialization_prefix.hpp>
#include <ikos/analyzer/analysis/execution_engine/native_thread_handles.hpp>
#include <ikos/analyzer/analysis/execution_engine/pthread_initialization.hpp>
#include <ikos/analyzer/checker/data_race.hpp>
#include <ikos/analyzer/util/log.hpp>
#include <map>
#include <set>
#include <tuple>

namespace ikos {
namespace analyzer {
namespace {
using core::ZNumber;
using Key = std::tuple< ar::Statement*, CallContext*, std::string >;
enum class Kind { Unknown, Null, Good, Static, Native };
struct Pointer {
  Kind kind = Kind::Unknown;
  ar::Value* object = nullptr;
  ZNumber offset{0};
  bool indexed = false;
  bool operator==(const Pointer& p) const {
    return kind == p.kind && object == p.object && offset == p.offset &&
           indexed == p.indexed;
  }
};
Pointer join(Pointer a, const Pointer& b) {
  // Good is conditional on a live non-NULL object; it asserts no validity.
  if (a.kind == Kind::Good && a.offset == 0 && b.kind == Kind::Null)
    return a;
  if (b.kind == Kind::Good && b.offset == 0 && a.kind == Kind::Null)
    return b;
  return a == b ? a : Pointer{};
}
struct State {
  std::map< ar::Value*, Pointer > pointers;
  std::set< ar::GlobalVariable* > held;
  bool concurrent = false;
  bool reachable = true;
  bool operator==(const State& s) const {
    return pointers == s.pointers && held == s.held &&
           concurrent == s.concurrent && reachable == s.reachable;
  }
  void join_with(const State& s) {
    for (auto it = pointers.begin(); it != pointers.end();) {
      auto found = s.pointers.find(it->first);
      auto p = found == s.pointers.end() ? Pointer{}
                                         : join(it->second, found->second);
      if (p.kind == Kind::Unknown)
        it = pointers.erase(it);
      else {
        it->second = p;
        ++it;
      }
    }
    std::set< ar::GlobalVariable* > common;
    std::set_intersection(held.begin(),
                          held.end(),
                          s.held.begin(),
                          s.held.end(),
                          std::inserter(common, common.end()));
    held = std::move(common);
    concurrent |= s.concurrent;
  }
};
struct Certificate {
  bool birth = false;
  ZNumber offset{0}, width{0};
  bool operator==(const Certificate& c) const {
    return birth == c.birth && offset == c.offset && width == c.width;
  }
};

bool direct_abi(ar::CallBase* call, ar::Function* target) {
  auto* source =
      call->has_frontend()
          ? llvm::dyn_cast< llvm::CallBase >(call->frontend< llvm::Value >())
          : nullptr;
  auto* original = source ? llvm::dyn_cast< llvm::Function >(
                                source->getCalledOperand()->stripPointerCasts())
                          : nullptr;
  if (!target || !target->has_frontend() || !original ||
      target->type()->is_var_arg() || original->isVarArg() ||
      target->frontend< llvm::Value >() != original ||
      source->getFunctionType() != original->getFunctionType() ||
      source->getCallingConv() != llvm::CallingConv::C ||
      original->getCallingConv() != llvm::CallingConv::C ||
      call->num_arguments() != target->num_parameters() ||
      source->arg_size() != call->num_arguments())
    return false;
  for (unsigned i = 0; i < call->num_arguments(); ++i) {
    if (call->argument(i)->type() != target->type()->param_type(i))
      return false;
    for (auto attr : {llvm::Attribute::ByVal,
                      llvm::Attribute::StructRet,
                      llvm::Attribute::InAlloca,
                      llvm::Attribute::Preallocated})
      if (source->paramHasAttr(i, attr) || original->hasParamAttribute(i, attr))
        return false;
  }
  return true;
}

bool mutex_api(ar::CallBase* call, const char* name) {
  auto* f = native_handle_callee(call);
  auto* source =
      call->has_frontend()
          ? llvm::dyn_cast< llvm::CallBase >(call->frontend< llvm::Value >())
          : nullptr;
  auto* original = source ? llvm::dyn_cast< llvm::Function >(
                                source->getCalledOperand()->stripPointerCasts())
                          : nullptr;
  if (original && source)
    for (auto attr : {llvm::Attribute::ByVal,
                      llvm::Attribute::StructRet,
                      llvm::Attribute::InAlloca,
                      llvm::Attribute::Preallocated})
      if (source->paramHasAttr(0, attr) || original->hasParamAttribute(0, attr))
        return false;
  const auto expected = std::string(name) == "pthread_mutex_lock"
                            ? ar::Intrinsic::PthreadMutexLock
                            : ar::Intrinsic::PthreadMutexUnlock;
  return f && f->is_declaration() && f->intrinsic_id() == expected &&
         call->num_arguments() == 1 &&
         call->argument(0)->type()->is_pointer() && original &&
         original->isDeclaration() && original->getName() == name &&
         !original->isVarArg() && original->arg_size() == 1 &&
         source->arg_size() == 1 &&
         original->getReturnType()->isIntegerTy(32) &&
         source->getType()->isIntegerTy(32) &&
         original->getFunctionType()->getParamType(0)->isPointerTy() &&
         source->getArgOperand(0)->getType()->isPointerTy() &&
         source->getArgOperand(0)->getType()->getPointerAddressSpace() == 0 &&
         original->getFunctionType()
                 ->getParamType(0)
                 ->getPointerAddressSpace() == 0 &&
         source->getFunctionType() == original->getFunctionType() &&
         source->getCallingConv() == llvm::CallingConv::C &&
         original->getCallingConv() == llvm::CallingConv::C;
}
// Range/effect evidence does not by itself prove the source calling ABI.
bool array_effect_abi(ar::CallBase* call) {
  auto* source =
      call->has_frontend()
          ? llvm::dyn_cast< llvm::CallBase >(call->frontend< llvm::Value >())
          : nullptr;
  auto* original = source ? llvm::dyn_cast< llvm::Function >(
                                source->getCalledOperand()->stripPointerCasts())
                          : nullptr;
  if (!original || !original->isDeclaration() || original->isVarArg() ||
      source->getFunctionType() != original->getFunctionType() ||
      source->arg_size() != original->arg_size() ||
      source->arg_size() != call->num_arguments())
    return false;
  for (unsigned i = 0; i < source->arg_size(); ++i) {
    auto* actual = source->getArgOperand(i)->getType();
    auto* formal = original->getFunctionType()->getParamType(i);
    if (!actual->isPointerTy() || !formal->isPointerTy() ||
        actual->getPointerAddressSpace() != 0 ||
        formal->getPointerAddressSpace() != 0)
      return false;
    for (auto attr : {llvm::Attribute::ByVal,
                      llvm::Attribute::StructRet,
                      llvm::Attribute::InAlloca,
                      llvm::Attribute::Preallocated})
      if (source->paramHasAttr(i, attr) || original->hasParamAttribute(i, attr))
        return false;
  }
  return true;
}
bool nondet_api(ar::CallBase* call) {
  auto* f = native_handle_callee(call);
  if (!f || !f->is_declaration() || call->num_arguments() != 0 ||
      !call->has_result() || !call->result()->type()->is_integer())
    return false;
  // Internal freeze symbols have no source call; their intrinsic is
  // authoritative.
  if (f->intrinsic_id() == ar::Intrinsic::IkosNonDet)
    return isa< ar::FunctionPointerConstant >(call->called());
  auto* source =
      call->has_frontend()
          ? llvm::dyn_cast< llvm::CallBase >(call->frontend< llvm::Value >())
          : nullptr;
  auto* original = source ? llvm::dyn_cast< llvm::Function >(
                                source->getCalledOperand()->stripPointerCasts())
                          : nullptr;
  auto* type = dyn_cast< ar::IntegerType >(call->result()->type());
  return f->name() == "__VERIFIER_nondet_int" && type->bit_width() == 32 &&
         type->is_signed() && original && original->isDeclaration() &&
         original->getName() == "__VERIFIER_nondet_int" &&
         original->arg_size() == 0 && source->arg_size() == 0 &&
         original->getReturnType()->isIntegerTy(32) &&
         source->getType()->isIntegerTy(32) &&
         original->getCallingConv() == llvm::CallingConv::C &&
         source->getCallingConv() == llvm::CallingConv::C;
}
bool abort_api(ar::CallBase* call) {
  auto* f = native_handle_callee(call);
  auto* source =
      call->has_frontend()
          ? llvm::dyn_cast< llvm::CallBase >(call->frontend< llvm::Value >())
          : nullptr;
  auto* original = source ? llvm::dyn_cast< llvm::Function >(
                                source->getCalledOperand()->stripPointerCasts())
                          : nullptr;
  return f && f->is_declaration() &&
         f->intrinsic_id() == ar::Intrinsic::LibcAbort &&
         call->num_arguments() == 0 && f->type()->return_type()->is_void() &&
         original && original->isDeclaration() &&
         original->getName() == "abort" && !original->isVarArg() &&
         original->arg_size() == 0 && source->arg_size() == 0 &&
         original->getReturnType()->isVoidTy() &&
         source->getType()->isVoidTy() &&
         source->getCallingConv() == llvm::CallingConv::C &&
         original->getCallingConv() == llvm::CallingConv::C;
}
} // namespace

class FirstPublication::Impl {
  Context& ctx;
  const std::vector< AccessRecord >& accesses;
  ar::StructType* layout = nullptr;
  ZNumber extent{0}, link{0}, pointer_width{0};
  std::set< ar::GlobalVariable* > roots, mutexes, root_arrays, effect_arrays;
  ar::GlobalVariable* lock = nullptr;
  std::set< ar::Function* > workers, active;
  std::set< ar::GlobalVariable* > concurrent_writes, unlocked_reads;
  std::map< ar::CallBase*, HeapInitializationDescription > births;
  std::set< std::pair< ar::CallBase*, CallContext* > > sources;
  std::map< Key, Certificate > certificates;
  std::set< Key > refused;
  std::string root, reason;
  bool valid = true;
  unsigned budget = 131072;

  bool fail(const char* message) {
    if (valid)
      reason = message;
    valid = false;
    return false;
  }
  bool spend() {
    return budget != 0 ? (--budget, true) : fail("finite protocol budget");
  }
  Pointer pointer(ar::Value* value, const State& state) const {
    auto it = state.pointers.find(value);
    if (it != state.pointers.end())
      return it->second;
    if (isa< ar::NullConstant >(value))
      return {Kind::Null, nullptr, ZNumber(0)};
    if (isa< ar::GlobalVariable >(value) || isa< ar::LocalVariable >(value))
      return {Kind::Static, value, ZNumber(0)};
    return {};
  }
  Key key(ar::Statement* statement, CallContext* context) const {
    return {statement, context, root};
  }
  void certify(ar::Statement* statement,
               CallContext* context,
               bool birth,
               const ZNumber& offset,
               const ZNumber& width) {
    auto k = key(statement, context);
    Certificate c{birth, offset, width};
    auto found = certificates.emplace(k, c);
    // Every fixed-point observation must support the same proposition.
    if (!found.second && !(found.first->second == c))
      refused.insert(k);
  }
  bool actual_lock(ar::Statement* statement, CallContext* context) const {
    auto id = core::IndexableTraits< MemoryLocation* >::index(
        ctx.mem_factory->get_global(lock));
    const auto k = offset_lock_key(id, ZNumber(0));
    bool seen = false;
    for (const auto& access : accesses) {
      if (access.stmt != statement || access.call_context != context ||
          access.thread_id != root)
        continue;
      seen = true;
      if (std::find(access.locks.begin(), access.locks.end(), k) ==
          access.locks.end())
        return false;
    }
    return seen;
  }
  bool locked(ar::Statement* statement,
              CallContext* context,
              const State& state) const {
    return state.held.count(lock) && actual_lock(statement, context);
  }
  bool array_range(ar::Statement* statement,
                   CallContext* context,
                   ar::GlobalVariable* object,
                   const ZNumber& width) const {
    const auto size = ctx.bundle->data_layout().alloc_size_in_bytes(
        object->type()->pointee());
    bool seen = false;
    for (const auto& a : accesses) {
      if (a.stmt != statement || a.call_context != context ||
          a.thread_id != root)
        continue;
      seen = true;
      if (!a.pts.is_set() || a.pts.size() != 1 ||
          *a.pts.begin() != ctx.mem_factory->get_global(object) ||
          a.width != width || a.offset.is_bottom() || a.offset.is_top() ||
          a.offset.lb().to_z_number() < 0 ||
          a.offset.ub().to_z_number() + width > size)
        return false;
    }
    return seen;
  }
  bool function(ar::Function*, CallContext*, State&, Pointer&);
  bool transfer(ar::Statement*, CallContext*, State&);
  bool summary(ar::CallBase*,
               CallContext*,
               State&,
               const HeapInitializationDescription&);
  bool inventory();
  bool closed_function_uses(ar::Function* f);
  void run();

public:
  Impl(Context& c, const std::vector< AccessRecord >& a) : ctx(c), accesses(a) {
    run();
  }
  ~Impl() {
    if (layout && !valid)
      log::debug("First publication unsupported: " + reason);
  }
  bool covered(const AccessRecord& a) const {
    if (a.is_atomic || !a.stmt || !a.pts.is_set() || a.pts.is_empty() ||
        a.width == 0 || (!isa< ar::Load >(a.stmt) && !isa< ar::Store >(a.stmt)))
      return false;
    auto it = certificates.find({a.stmt, a.call_context, a.thread_id});
    if (it == certificates.end() || refused.count(it->first) ||
        it->second.width != ZNumber(a.width))
      return false;
    for (auto* ml : a.pts) {
      auto* object = dyn_cast< DynAllocMemoryLocation >(ml);
      if (!object || !sources.count({object->call(), object->context()}))
        return false;
    }
    return true;
  }
  bool ordered(const AccessRecord& a, const AccessRecord& b) const {
    if (!valid || !lock || !covered(a) || !covered(b))
      return false;
    const auto& x = certificates.at({a.stmt, a.call_context, a.thread_id});
    const auto& y = certificates.at({b.stmt, b.call_context, b.thread_id});
    return x.birth || y.birth;
  }
  JsonDict info() const {
    return {{"publication_contract", "closed-first-publication-v1"},
            {"scope", "birth/birth or birth/acquired live-heap access pairs"},
            {"publication_mutex", lock ? lock->name() : ""},
            {"object_extent", extent.str()},
            {"link_offset", link.str()}};
  }
};

bool FirstPublication::Impl::closed_function_uses(ar::Function* f) {
  if (!f->has_frontend())
    return fail("missing original function");
  auto* original = f->frontend< llvm::Function >();
  std::vector< llvm::Value* > pending{original};
  std::set< llvm::Value* > seen;
  while (!pending.empty()) {
    auto* value = pending.back();
    pending.pop_back();
    if (!spend())
      return false;
    if (!seen.insert(value).second)
      continue;
    for (auto* user : value->users()) {
      if (llvm::isa< llvm::BitCastOperator >(user)) {
        pending.push_back(user);
        continue;
      }
      auto* call = llvm::dyn_cast< llvm::CallBase >(user);
      if (!call)
        return fail("escaped function address");
      if (call->getCalledOperand() == value)
        continue;
      bool allowed = false;
      for (const auto& item : ctx.native_handle_accesses) {
        auto* create = dyn_cast< ar::CallBase >(item.first);
        if (!create || !create->has_frontend() ||
            create->frontend< llvm::Value >() != call ||
            !ctx.native_handle_effect_certificates.count(create))
          continue;
        allowed =
            native_handle_api(call, true) && call->getArgOperand(2) == value;
      }
      if (!allowed)
        return fail("unclosed callback use");
    }
  }
  return true;
}

bool FirstPublication::Impl::inventory() {
  // Structural layout: one self link and scalar payload. No source identifiers.
  for (auto it = ctx.bundle->global_begin(); it != ctx.bundle->global_end();
       ++it) {
    auto* global = *it;
    auto* array = dyn_cast< ar::ArrayType >(global->type()->pointee());
    auto* type = dyn_cast< ar::PointerType >(array ? array->element_type()
                                                   : global->type()->pointee());
    auto* object = type ? dyn_cast< ar::StructType >(type->pointee()) : nullptr;
    if (!object)
      continue;
    ZNumber field{0};
    unsigned links = 0;
    bool supported = true;
    for (const auto& f : object->layout()) {
      if (auto* p = dyn_cast< ar::PointerType >(f.type)) {
        supported &= p->pointee() == object;
        field = f.offset;
        ++links;
      } else
        supported &= f.type->is_integer() || f.type->is_float();
    }
    if (!supported || links != 1)
      continue;
    if (layout && layout != object)
      return fail("multiple graph layouts");
    layout = object;
    link = field;
    extent = ctx.bundle->data_layout().alloc_size_in_bytes(object);
    if (!global->is_definition() || global->is_thread_local() ||
        !global->has_frontend())
      return fail("root definition");
    auto* source = global->frontend< llvm::GlobalVariable >();
    if (!source->hasInitializer() || !source->getInitializer()->isNullValue() ||
        source->isExternallyInitialized() || source->getAddressSpace() != 0 ||
        source->isConstant())
      return fail("root initialization");
    if (array) {
      auto* original =
          llvm::dyn_cast< llvm::ArrayType >(source->getValueType());
      if (!original || array->num_elements() <= 0 ||
          array->num_elements() != ZNumber(original->getNumElements()) ||
          ctx.bundle->data_layout().alloc_size_in_bytes(array) !=
              4 * array->num_elements() ||
          source->getParent()->getDataLayout().getTypeAllocSize(original) !=
              4 * original->getNumElements() ||
          4 * array->num_elements() >= (ZNumber(1) << 32))
        return fail("root array layout");
      root_arrays.insert(global);
    }
    roots.insert(global);
  }
  if (!layout)
    return fail("no scalar graph roots");
  pointer_width = ctx.bundle->data_layout().pointers.bit_width / 8;
  if (pointer_width != 4 || extent <= 0 || link < 0 ||
      link + pointer_width > extent || ctx.opts.entry_points.size() != 1 ||
      ctx.opts.entry_points.front()->name() != "main" ||
      ctx.opts.entry_points.front()->num_parameters() != 0 ||
      !ctx.opts.no_init_globals.empty() || ctx.concurrent_env.is_dirty() ||
      !ctx.concurrent_env.unmodeled_effects().empty() ||
      !ctx.concurrent_env.unmodeled_thread_creations().empty() ||
      !ctx.native_handles_only ||
      ctx.bundle->global_or_null("ar.global_ctors") ||
      ctx.bundle->global_or_null("ar.global_dtors"))
    return fail("global closure");
  auto* main = ctx.opts.entry_points.front();
  if (!main->has_frontend() ||
      !main->frontend< llvm::Function >()->use_empty() ||
      !mutex_effect_pointer_representations_closed(
          *main->frontend< llvm::Function >()->getParent()))
    return fail("entry or pointer representation closure");
  // Clang can lower an aggregate-initialized union to an anonymous storage
  // structure. Check the actual typed API operand and the complete zero bytes.
  for (auto fit = ctx.bundle->function_begin();
       fit != ctx.bundle->function_end();
       ++fit) {
    if (!(*fit)->is_definition())
      continue;
    for (auto* block : *(*fit)->body())
      for (auto* statement : *block) {
        auto* call = dyn_cast< ar::CallBase >(statement);
        if (!call || (!mutex_api(call, "pthread_mutex_lock") &&
                      !mutex_api(call, "pthread_mutex_unlock")))
          continue;
        auto* source =
            llvm::cast< llvm::CallBase >(call->frontend< llvm::Value >());
        auto* global = llvm::dyn_cast< llvm::GlobalVariable >(
            source->getArgOperand(0)->stripPointerCasts());
        auto* declared = llvm::dyn_cast< llvm::StructType >(
            source->getArgOperand(0)->getType()->getPointerElementType());
        if (!global || !declared || declared->isOpaque() ||
            !declared->isSized() || !declared->hasName() ||
            declared->getName() != "union.pthread_mutex_t" ||
            global->isConstant() || global->isThreadLocal() ||
            global->isExternallyInitialized() ||
            global->getAddressSpace() != 0 || !global->hasInitializer() ||
            !global->getInitializer()->isNullValue())
          continue;
        const auto& native = global->getParent()->getDataLayout();
        const auto alignment =
            global->getAlign()
                ? global->getAlign()->value()
                : native.getABITypeAlign(global->getValueType()).value();
        if (native.getTypeAllocSize(declared) != 24 ||
            native.getTypeAllocSize(global->getValueType()) != 24 ||
            alignment < 4)
          continue;
        for (auto it = ctx.bundle->global_begin();
             it != ctx.bundle->global_end();
             ++it) {
          auto* object = *it;
          if (object->has_frontend() &&
              object->frontend< llvm::Value >() == global &&
              object->is_definition() && !object->is_thread_local() &&
              exact_handle_slot(call->argument(0), object) &&
              ctx.bundle->data_layout().alloc_size_in_bytes(
                  object->type()->pointee()) == 24) {
            if (roots.count(object))
              return fail("overlapping graph and mutex storage");
            mutexes.insert(object);
          }
        }
      }
  }
  for (auto fit = ctx.bundle->function_begin();
       fit != ctx.bundle->function_end();
       ++fit) {
    auto* f = *fit;
    if (!f->is_definition())
      continue;
    if (!closed_function_uses(f))
      return false;
    for (auto* block : *f->body())
      for (auto* statement : *block) {
        if (!spend())
          return false;
        auto* call = dyn_cast< ar::CallBase >(statement);
        if (!call)
          continue;
        auto* target = native_handle_callee(call);
        if (!target)
          return fail("indirect call");
        if (target->is_definition()) {
          if (!direct_abi(call, target))
            return fail("helper ABI");
          continue;
        }
        if (target->intrinsic_id() == ar::Intrinsic::LibcMalloc) {
          auto* count = call->num_arguments() == 1
                            ? dyn_cast< ar::IntegerConstant >(call->argument(0))
                            : nullptr;
          HeapInitializationDescription description;
          if (!count || count->value().to_z_number() != extent ||
              !prove_heap_initialization_prefix(call, link, ctx, &description))
            return fail("incomplete birth prefix");
          births.emplace(call, std::move(description));
        } else if (ctx.native_handle_effect_certificates.count(call)) {
          if (call->num_arguments() != 4 || call->code() != main->body() ||
              !isa< ar::NullConstant >(call->argument(1)) ||
              !isa< ar::NullConstant >(call->argument(3)))
            return fail("thread create transfer");
          auto* child =
              dyn_cast< ar::FunctionPointerConstant >(call->argument(2));
          if (!child || !child->function()->is_definition() ||
              child->function() == main)
            return fail("callback identity");
          workers.insert(child->function());
        } else if (ctx.mutex_effect_certificates.count(call)) {
          auto object = mutex_array_effect_object(call);
          if (!object || !array_effect_abi(call) ||
              roots.count(object.global) || mutexes.count(object.global))
            return fail("array mutex disjointness");
          effect_arrays.insert(object.global);
        } else if (!ctx.native_handle_joins.count(call) && !nondet_api(call) &&
                   !abort_api(call) && !constant_assertion_effect(call) &&
                   !mutex_api(call, "pthread_mutex_lock") &&
                   !mutex_api(call, "pthread_mutex_unlock")) {
          log::debug("First publication external target: " + target->name());
          return fail("unclosed external effect");
        }
      }
  }
  if (births.empty())
    return fail("no described allocations");
  if (workers.empty())
    return fail("no native callbacks");
  if (mutexes.empty())
    return fail("no literal normal mutexes");
  return true;
}

bool FirstPublication::Impl::summary(
    ar::CallBase* allocation,
    CallContext* cc,
    State& state,
    const HeapInitializationDescription& description) {
  if (state.concurrent && !description.relays.empty())
    return fail("concurrent startup relay");
  sources.insert({allocation, cc});
  bool started = false;
  for (auto* statement : *allocation->parent()) {
    if (statement == allocation)
      started = true;
    if (!started)
      continue;
    if (auto* result = statement->result_or_null())
      state.pointers.erase(result);
    if (statement == description.completion)
      break;
  }
  for (const auto& alias : description.aliases)
    state.pointers[alias.first] = {Kind::Good, nullptr, alias.second};
  for (const auto& write : description.writes) {
    auto* frame =
        write.helper
            ? ctx.call_context_factory->get_context(cc, description.helper)
            : cc;
    certify(write.statement, frame, true, write.offset, write.width);
  }
  // These writes are proven before every create, never a concurrent root write.
  for (auto* global : description.relays)
    if (!roots.count(global))
      return fail("foreign startup relay");
  return true;
}

bool FirstPublication::Impl::transfer(ar::Statement* stmt,
                                      CallContext* cc,
                                      State& state) {
  if (auto* copy = dyn_cast< ar::Assignment >(stmt)) {
    auto p = pointer(copy->operand(), state);
    state.pointers.erase(copy->result());
    if (copy->result()->type()->is_pointer())
      state.pointers[copy->result()] = p;
    return true;
  }
  if (auto* cast = dyn_cast< ar::UnaryOperation >(stmt)) {
    auto p = pointer(cast->operand(), state);
    if (cast->operand()->type()->is_pointer() !=
        cast->result()->type()->is_pointer())
      return fail("pointer representation conversion");
    state.pointers.erase(cast->result());
    if (cast->result()->type()->is_pointer()) {
      if (cast->op() != ar::UnaryOperation::Bitcast)
        return fail("nontransparent pointer conversion");
      state.pointers[cast->result()] = p;
    }
    return true;
  }
  if (auto* shift = dyn_cast< ar::PointerShift >(stmt)) {
    auto p = pointer(shift->pointer(), state);
    for (auto it = shift->term_begin(); it != shift->term_end(); ++it) {
      auto* index = dyn_cast< ar::IntegerConstant >((*it).second);
      if (!index) {
        // Only already closed native pthread_t storage may have a symbolic
        // index.
        bool native = false;
        for (const auto& item : ctx.native_handle_accesses)
          native |= item.second.storage == p.object;
        auto* global = dyn_cast_or_null< ar::GlobalVariable >(p.object);
        const bool array =
            root_arrays.count(global) || effect_arrays.count(global);
        if ((!native && !array) ||
            (p.kind != Kind::Static && p.kind != Kind::Native))
          return fail("symbolic graph offset");
        if (root_arrays.count(global) &&
            ((*it).first.to_z_number() % pointer_width != 0 ||
             p.offset % pointer_width != 0))
          return fail("unaligned root index");
        p.indexed = true;
        if (native)
          p.kind = Kind::Native;
      } else
        p.offset += (*it).first.to_z_number() * index->value().to_z_number();
    }
    if (root_arrays.count(dyn_cast_or_null< ar::GlobalVariable >(p.object)) &&
        p.offset % pointer_width != 0)
      return fail("partial root pointer offset");
    if (p.kind == Kind::Null && p.offset != 0)
      return fail("nonzero arithmetic from NULL");
    if (p.kind == Kind::Good && (p.offset < 0 || p.offset > extent))
      return fail("heap pointer range");
    state.pointers.erase(shift->result());
    state.pointers[shift->result()] = p;
    return true;
  }
  if (auto* op = dyn_cast< ar::BinaryOperation >(stmt)) {
    if (!op->result()->type()->is_integer() &&
        !op->result()->type()->is_float())
      return fail("nonscalar arithmetic");
    state.pointers.erase(op->result());
    return true;
  }
  if (isa< ar::Comparison >(stmt))
    return true;
  if (auto* allocation = dyn_cast< ar::Allocate >(stmt)) {
    state.pointers[allocation->result()] = {Kind::Static,
                                            allocation->result(),
                                            ZNumber(0)};
    return true;
  }
  if (isa< ar::Load >(stmt) || isa< ar::Store >(stmt)) {
    auto* load = dyn_cast< ar::Load >(stmt);
    auto* store = dyn_cast< ar::Store >(stmt);
    auto* operand = load ? load->operand() : store->pointer();
    auto* type = load ? load->result()->type() : store->value()->type();
    auto width = ctx.bundle->data_layout().store_size_in_bytes(type);
    auto p = pointer(operand, state);
    Pointer result;
    if ((load && (load->is_atomic() || load->is_volatile())) ||
        (store && (store->is_atomic() || store->is_volatile())))
      return fail("atomic/volatile memory");
    if (p.kind == Kind::Static &&
        roots.count(dyn_cast_or_null< ar::GlobalVariable >(p.object))) {
      auto* global = cast< ar::GlobalVariable >(p.object);
      if (!type->is_pointer() || width != pointer_width)
        return fail("root byte representation");
      if (root_arrays.count(global)) {
        if (p.offset % pointer_width != 0 ||
            !array_range(stmt, cc, global, width))
          return fail("root array range");
      } else if (p.indexed || p.offset != 0)
        return fail("root slot offset");
      if (load) {
        if (state.concurrent && !locked(stmt, cc, state))
          unlocked_reads.insert(global);
        result.kind = Kind::Good;
      } else {
        auto rhs = pointer(store->value(), state);
        if (rhs.kind != Kind::Null &&
            (rhs.kind != Kind::Good || rhs.offset != 0))
          return fail("unknown root value");
        if (state.concurrent) {
          concurrent_writes.insert(global);
          if (!locked(stmt, cc, state))
            return fail("unlocked root writer");
        }
      }
    } else if (p.kind == Kind::Good) {
      if (width <= 0 || p.offset < 0 || p.offset + width > extent)
        return fail("heap byte range");
      if (type->is_pointer()) {
        if (p.offset != link || width != pointer_width ||
            !locked(stmt, cc, state))
          return fail("unlocked or partial graph edge");
        if (load)
          result.kind = Kind::Good;
        else {
          auto rhs = pointer(store->value(), state);
          if (rhs.kind != Kind::Null &&
              (rhs.kind != Kind::Good || rhs.offset != 0))
            return fail("uninitialized graph value");
        }
      } else if ((!type->is_integer() && !type->is_float()) ||
                 (p.offset < link + pointer_width && link < p.offset + width))
        return fail("payload overlaps link");
      certify(stmt, cc, false, p.offset, width);
    } else if ((p.kind == Kind::Static || p.kind == Kind::Native) &&
               (type->is_integer() || type->is_float())) {
      // Static scalar storage cannot receive heap pointers; require real bounds
      // so an oversized access cannot modify a neighboring root or mutex.
      auto native = ctx.native_handle_accesses.find(stmt);
      if (native == ctx.native_handle_accesses.end()) {
        auto* object = dyn_cast_or_null< ar::Variable >(p.object);
        if (!object || p.indexed ||
            effect_arrays.count(
                dyn_cast_or_null< ar::GlobalVariable >(p.object)) ||
            mutexes.count(dyn_cast_or_null< ar::GlobalVariable >(p.object)) ||
            p.offset < 0 ||
            p.offset + width >
                ctx.bundle->data_layout().alloc_size_in_bytes(
                    cast< ar::PointerType >(object->type())->pointee()))
          return fail("static scalar footprint");
      }
    } else
      return fail("unknown memory provenance");
    if (load) {
      state.pointers.erase(load->result());
      if (type->is_pointer())
        state.pointers[load->result()] = result;
    }
    return true;
  }
  if (auto* call = dyn_cast< ar::CallBase >(stmt)) {
    auto* target = native_handle_callee(call);
    if (!target)
      return fail("unknown call transfer");
    Pointer result;
    if (target->is_definition()) {
      if (!direct_abi(call, target))
        return fail("helper transfer ABI");
      std::vector< Pointer > actuals;
      for (unsigned i = 0; i < call->num_arguments(); ++i)
        actuals.push_back(pointer(call->argument(i), state));
      for (unsigned i = 0; i < call->num_arguments(); ++i) {
        state.pointers.erase(target->param(i));
        if (target->param(i)->type()->is_pointer())
          state.pointers[target->param(i)] = actuals[i];
      }
      auto* child = ctx.call_context_factory->get_context(cc, call);
      if (!function(target, child, state, result))
        return false;
    } else if (ctx.mutex_effect_certificates.count(call)) {
      auto object = mutex_array_effect_object(call);
      if (!object || !array_effect_abi(call) || object.global == lock ||
          !effect_arrays.count(object.global))
        return fail("array effect scope");
      // This independent complete-range certificate grants no graph lock.
    } else if (mutex_api(call, "pthread_mutex_lock") ||
               mutex_api(call, "pthread_mutex_unlock")) {
      auto p = pointer(call->argument(0), state);
      auto* object = dyn_cast_or_null< ar::GlobalVariable >(p.object);
      if (p.kind != Kind::Static || p.offset != 0 || !mutexes.count(object))
        return fail("literal normal mutex scope");
      if (mutex_api(call, "pthread_mutex_lock")) {
        if (!state.held.insert(object).second)
          return fail("recursive lock");
      } else if (!state.held.erase(object))
        return fail("unheld unlock");
    } else if (ctx.native_handle_effect_certificates.count(call)) {
      state.concurrent = true;
    } else if (!ctx.native_handle_joins.count(call) && !nondet_api(call)) {
      return fail("unsupported transfer effect");
    }
    if (call->has_result()) {
      state.pointers.erase(call->result());
      if (call->result()->type()->is_pointer())
        state.pointers[call->result()] = result;
    }
    return true;
  }
  return fail("unsupported AR statement");
}

bool FirstPublication::Impl::function(ar::Function* f,
                                      CallContext* cc,
                                      State& state,
                                      Pointer& returned) {
  if (!valid || !f->is_definition() || !active.insert(f).second)
    return fail("recursive helper");
  std::map< ar::BasicBlock*, State > entries;
  std::deque< ar::BasicBlock* > pending;
  auto* body = f->body();
  entries.emplace(body->entry_block(), state);
  pending.push_back(body->entry_block());
  State exit;
  Pointer result;
  bool has_return = false;
  while (!pending.empty() && valid) {
    auto* block = pending.front();
    pending.pop_front();
    State current = entries.at(block);
    bool terminal = false;
    ar::Statement* skip_until = nullptr;
    for (auto* statement : *block) {
      if (!spend())
        return false;
      if (skip_until) {
        if (statement == skip_until)
          skip_until = nullptr;
        continue;
      }
      if (auto* call = dyn_cast< ar::CallBase >(statement)) {
        auto birth = births.find(call);
        if (birth != births.end()) {
          if (!summary(call, cc, current, birth->second))
            return false;
          skip_until = birth->second.completion;
          continue;
        }
        if (abort_api(call) || constant_assertion_effect(call)) {
          terminal = true;
          break;
        }
      }
      if (auto* ret = dyn_cast< ar::ReturnValue >(statement)) {
        auto p =
            ret->has_operand() ? pointer(ret->operand(), current) : Pointer{};
        if (!has_return) {
          exit = current;
          result = p;
          has_return = true;
        } else {
          exit.join_with(current);
          result = join(result, p);
        }
        terminal = true;
        break;
      }
      if (isa< ar::Unreachable >(statement)) {
        terminal = true;
        break;
      }
      if (!transfer(statement, cc, current)) {
        if (log::is_enabled_for(LogLevel::Debug)) {
          auto message = log::msg();
          message << "First publication stopped at: ";
          statement->dump(message.stream());
          message << "\n";
        }
        return false;
      }
      if (!current.reachable) {
        terminal = true;
        break;
      }
    }
    if (skip_until)
      return fail("birth completion missing");
    if (terminal)
      continue;
    for (auto it = block->successor_begin(); it != block->successor_end();
         ++it) {
      auto added = entries.emplace(*it, current);
      if (added.second)
        pending.push_back(*it);
      else {
        State merged = added.first->second;
        merged.join_with(current);
        if (!(merged == added.first->second)) {
          added.first->second = std::move(merged);
          pending.push_back(*it);
        }
      }
    }
  }
  active.erase(f);
  if (!valid)
    return false;
  if (!has_return) {
    state.reachable = false;
    returned = {};
    return true;
  }
  // Capture return first, then kill callee-local bindings. Facts refer to
  // initialized objects, never to the reusable formal/result identities.
  for (auto it = f->param_begin(); it != f->param_end(); ++it)
    exit.pointers.erase(*it);
  for (auto* block : *body)
    for (auto* statement : *block)
      if (auto* value = statement->result_or_null())
        exit.pointers.erase(value);
  state = std::move(exit);
  returned = result;
  return true;
}

void FirstPublication::Impl::run() {
  if (!inventory()) {
    valid = false;
    return;
  }
  std::vector< ar::GlobalVariable* > ordered(mutexes.begin(), mutexes.end());
  std::sort(ordered.begin(), ordered.end(), [](auto* a, auto* b) {
    return a->name() < b->name();
  });
  std::vector< ar::Function* > threads(workers.begin(), workers.end());
  std::sort(threads.begin(), threads.end(), [](auto* a, auto* b) {
    return a->name() < b->name();
  });
  auto* cc = ctx.call_context_factory->get_empty();
  for (auto* candidate : ordered) {
    lock = candidate;
    valid = true;
    reason.clear();
    budget = 131072;
    certificates.clear();
    refused.clear();
    concurrent_writes.clear();
    unlocked_reads.clear();
    sources.clear();
    active.clear();
    root = "main";
    State main;
    Pointer ignored;
    if (!function(ctx.opts.entry_points.front(), cc, main, ignored))
      continue;
    for (auto* worker : threads) {
      if (worker->num_parameters() != 1 ||
          !worker->param(0)->type()->is_pointer() ||
          !worker->type()->return_type()->is_pointer()) {
        fail("worker ABI");
        break;
      }
      root = worker->name();
      State entry;
      entry.concurrent = true;
      entry.pointers[worker->param(0)] = {Kind::Null, nullptr, ZNumber(0)};
      if (!function(worker, cc, entry, ignored))
        break;
    }
    if (!valid)
      continue;
    for (auto* global : unlocked_reads)
      if (concurrent_writes.count(global))
        fail("unlocked read of mutable root");
    if (valid)
      return;
  }
  valid = false;
}

FirstPublication::FirstPublication(Context& ctx,
                                   const std::vector< AccessRecord >& a)
    : _impl(new Impl(ctx, a)) {}
FirstPublication::~FirstPublication() = default;
bool FirstPublication::ordered(const AccessRecord& a,
                               const AccessRecord& b) const {
  return _impl->ordered(a, b);
}
JsonDict FirstPublication::info() const {
  return _impl->info();
}
} // namespace analyzer
} // namespace ikos
