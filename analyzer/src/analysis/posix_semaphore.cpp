// Copyright and license: see context.hpp.
#include <ikos/analyzer/analysis/posix_semaphore.hpp>
#include <ikos/analyzer/analysis/source_call_binding.hpp>
#include <ikos/analyzer/analysis/execution_engine/heap_pointer_effects.hpp>
#include <ikos/analyzer/analysis/execution_engine/allocation_continuation.hpp>
#include <llvm/IR/CFG.h>
#include <algorithm>
#include <set>

namespace ikos {
namespace analyzer {
class PosixSemaphore::Impl {
  Context& ctx;
  PthreadObjectLayout abi;
  std::map< llvm::Value*, ar::Statement* > originals;
  std::map< llvm::GlobalVariable*, ar::GlobalVariable* > globals;
  std::map< llvm::Function*, ar::Function* > functions;
  unsigned budget = 32768;
  bool step() { if (!budget) return false; --budget; return true; }
  llvm::Function* callee(llvm::CallBase* c) const {
    return llvm::dyn_cast< llvm::Function >(c->getCalledOperand()->stripPointerCasts());
  }
  ar::CallBase* binding(llvm::CallBase* c) const {
    auto i = originals.find(c);
    return i == originals.end() ? nullptr : source_call_binding(c, i->second);
  }
  bool pure(llvm::CallBase* c) const {
    auto* a = binding(c); auto* f = callee(c);
    if (!a || !f || !f->isDeclaration()) return false;
    auto* t = native_handle_callee(a);
    if (f->getName() == "abort")
      return t->intrinsic_id() == ar::Intrinsic::LibcAbort && c->arg_empty() && c->getType()->isVoidTy();
    if (!c->arg_empty() || !c->getType()->isIntegerTy()) return false;
    if (f->getName() == "__VERIFIER_nondet_int") return c->getType()->isIntegerTy(32);
    return t->intrinsic_id() == ar::Intrinsic::IkosNonDet && c->getMetadata("ikos.freeze_uninit") &&
        ((f->getName() == "__ikos_nondet_int" && c->getType()->isIntegerTy(32)) ||
         (f->getName() == "__ikos_nondet_int64" && c->getType()->isIntegerTy(64)));
  }
  bool handle(llvm::CallBase* c, bool create) const {
    auto* a = binding(c);
    if (!a || !native_handle_api(c, create)) return false;
    auto* t = native_handle_callee(a);
    if (t->name() != (create ? "pthread_create" : "pthread_join") ||
        !isa< ar::NullConstant >(a->argument(1))) return false;
    if (!create) return true;
    auto* entry = dyn_cast< ar::FunctionPointerConstant >(a->argument(2));
    return entry && entry->function()->has_frontend() &&
        entry->function()->frontend< llvm::Value >() == c->getArgOperand(2)->stripPointerCasts() &&
        isa< ar::NullConstant >(a->argument(3));
  }
  bool heap(llvm::CallBase* allocation, llvm::Function* main) {
    auto* a = binding(allocation);
    if (!a || allocation->getFunction() != main ||
        allocation->arg_size() != 1 || !allocation->getArgOperand(0)->getType()->isIntegerTy(abi.word * 8) ||
        !allocation->getType()->isPointerTy() ||
        native_handle_callee(a)->intrinsic_id() != ar::Intrinsic::LibcMalloc) return false;
    // Preserve single execution even when size/failure guards split main.
    std::vector< llvm::BasicBlock* > blocks;
    std::set< llvm::BasicBlock* > visited;
    for (auto* b : llvm::successors(allocation->getParent())) blocks.push_back(b);
    while (!blocks.empty()) {
      auto* b = blocks.back(); blocks.pop_back();
      if (!step() || b == allocation->getParent()) return false;
      if (!visited.insert(b).second) continue;
      for (auto* next : llvm::successors(b)) blocks.push_back(next);
    }
    std::vector< llvm::Value* > todo{allocation}; std::set< llvm::Value* > seen;
    std::set< llvm::CallBase* > closed_handles, closed_allocations{allocation};
    while (!todo.empty()) {
      auto* v = todo.back(); todo.pop_back();
      if (!step()) return false;
      if (!seen.insert(v).second) continue;
      for (auto* u : v->users()) {
        if (auto* cast = llvm::dyn_cast< llvm::BitCastOperator >(u)) { todo.push_back(cast); continue; }
        if (auto* gep = llvm::dyn_cast< llvm::GEPOperator >(u)) {
          if (gep->getPointerOperand() != v || !gep->getSourceElementType()->isIntegerTy(abi.word * 8)) return false;
          todo.push_back(gep); continue;
        }
        if (auto* compare = llvm::dyn_cast< llvm::ICmpInst >(u)) {
          auto* other = compare->getOperand(compare->getOperand(0) == v ? 1 : 0);
          auto* branch = llvm::dyn_cast< llvm::BranchInst >(allocation->getParent()->getTerminator());
          ar::Comparison* guard = nullptr;
          if (!compare->isEquality() || !llvm::isa< llvm::ConstantPointerNull >(other) ||
              v->stripPointerCasts() != allocation || compare->getParent() != allocation->getParent() ||
              !compare->hasOneUse() || !branch || !branch->isConditional() ||
              branch->getCondition() != compare ||
              !heap_initialization_successor(a->parent(), guard) || !guard) return false;
          auto* value = dyn_cast< ar::InternalVariable >(
              isa< ar::NullConstant >(guard->left()) ? guard->right() : guard->left());
          if (!value || unique_def(value) != a) return false;
          continue;
        }
        if (auto* load = llvm::dyn_cast< llvm::LoadInst >(u)) {
          if (load->getPointerOperand() != v || !load->getType()->isIntegerTy(abi.word * 8) ||
              load->isAtomic() || load->isVolatile() || load->getFunction() != main) return false;
          for (auto* use : load->users()) {
            auto* c = llvm::dyn_cast< llvm::CallBase >(use);
            if (!c || c->arg_empty() || c->getArgOperand(0) != load || !handle(c, false)) return false;
            closed_handles.insert(c);
          }
          continue;
        }
        auto* c = llvm::dyn_cast< llvm::CallBase >(u);
        if (!c || c->arg_empty() || c->getFunction() != main || !binding(c) || c->getArgOperand(0) != v) return false;
        if (handle(c, true)) closed_handles.insert(c);
        else {
          auto* f = callee(c); auto* t = native_handle_callee(binding(c));
          if (!f || f->getName() != "free" || t->intrinsic_id() != ar::Intrinsic::LibcFree ||
              c->arg_size() != 1 || !c->getType()->isVoidTy() ||
              v->stripPointerCasts() != allocation) return false;
          closed_allocations.insert(c);
        }
      }
    }
    handles.insert(closed_handles.begin(), closed_handles.end());
    allocations.insert(closed_allocations.begin(), closed_allocations.end());
    return true;
  }
  bool semaphore(llvm::GlobalVariable* g, llvm::Function* main) {
    auto* type = llvm::dyn_cast< llvm::StructType >(g->getValueType());
    auto* module = main->getParent();
    const unsigned extent = abi.word == 4 ? 16 : 32;
    if (!type || type->isOpaque() || !type->hasName() || type->getName() != "union.sem_t" ||
        !globals.count(g) || !globals.at(g)->is_definition() || globals.at(g)->is_thread_local() || g->isConstant() || g->isThreadLocal() || g->isExternallyInitialized() ||
        g->getAddressSpace() != 0 || !g->hasInitializer() || !g->getInitializer()->isNullValue() ||
        module->getDataLayout().getTypeAllocSize(type) != extent ||
        module->getDataLayout().getTypeStoreSize(type) != extent ||
        module->getDataLayout().getABITypeAlign(type).value() != abi.word ||
        (g->getAlign() ? g->getAlign()->value() : module->getDataLayout().getABITypeAlign(type).value()) < abi.word ||
        ctx.bundle->data_layout().alloc_size_in_bytes(globals.at(g)->type()->pointee()) != extent ||
        ctx.bundle->data_layout().store_size_in_bytes(globals.at(g)->type()->pointee()) != extent) return false;
    llvm::CallBase* init = nullptr;
    bool binary_permit = false;
    Token token{g, {}}; std::map< ar::CallBase*, ar::GlobalVariable* > candidate;
    std::vector< llvm::Value* > todo{g}; std::set< llvm::Value* > seen;
    while (!todo.empty()) {
      auto* v = todo.back(); todo.pop_back();
      if (!step()) return false;
      if (!seen.insert(v).second) continue;
      for (auto* u : v->users()) {
        if (llvm::isa< llvm::BitCastOperator >(u)) { todo.push_back(u); continue; }
        auto* c = llvm::dyn_cast< llvm::CallBase >(u);
        auto* a = c ? binding(c) : nullptr; auto* f = c ? callee(c) : nullptr;
        if (!a || !f->isDeclaration() || !c->getType()->isIntegerTy(32) ||
            c->arg_empty() || c->getArgOperand(0) != v ||
            !exact_handle_slot(a->argument(0), globals.at(g))) return false;
        const auto name = f->getName();
        if (name == "sem_init") {
          if (init || c->arg_size() != 3 || c->getFunction() != main ||
              c->getParent() != &main->getEntryBlock()) return false;
          for (unsigned n = 1; n < 3; ++n) {
            auto* k = llvm::dyn_cast< llvm::ConstantInt >(c->getArgOperand(n));
            auto* ak = dyn_cast< ar::IntegerConstant >(a->argument(n));
            if (!k || k->getBitWidth() != 32 || !ak || ak->value().bit_width() != 32) return false;
            const auto value = k->getZExtValue();
            if ((n == 1 && value != 0) || (n == 2 && value > 2147483647) ||
                ak->value().to_z_number() != core::ZNumber(value)) return false;
            if (n == 2) binary_permit = value == 1;
          }
          init = c;
        } else if ((name == "sem_wait" || name == "sem_post") && c->arg_size() == 1)
          token.operations.emplace(c, name == "sem_wait");
        else return false;
        candidate.emplace(a, globals.at(g));
      }
    }
    if (!init || token.operations.empty()) return false;
    // The unique initialization dominates all potential worker/helper entry.
    for (auto& i : main->getEntryBlock()) {
      if (&i == init) break;
      auto* c = llvm::dyn_cast< llvm::CallBase >(&i);
      if (c && !llvm::isa< llvm::DbgInfoIntrinsic >(c) && !pure(c)) return false;
    }
    effects.insert(candidate.begin(), candidate.end());
    if (binary_permit) tokens.push_back(std::move(token));
    return true;
  }
  bool build() {
    if (!ctx.opts.enable_thread_modular || ctx.opts.procedural == Procedural::Intraprocedural ||
        ctx.opts.entry_points.size() != 1 || ctx.opts.entry_points.front()->name() != "main" ||
        ctx.opts.entry_points.front()->num_parameters() != 0 || !ctx.opts.entry_points.front()->has_frontend() ||
        !ctx.opts.no_init_globals.empty() ||
        ctx.bundle->global_or_null("ar.global_ctors") || ctx.bundle->global_or_null("ar.global_dtors")) return false;
    auto* main = ctx.opts.entry_points.front()->frontend< llvm::Function >();
    abi = pthread_object_layout(ctx.bundle, main->getParent());
    if (!abi) return false;
    bool potential = false;
    for (auto& g : main->getParent()->globals()) {
      if (g.isDeclaration() && !g.use_empty()) return false;
      auto* t = llvm::dyn_cast< llvm::StructType >(g.getValueType());
      potential |= t && t->hasName() && t->getName() == "union.sem_t";
    }
    if (!potential) return false;
    if (!main->use_empty() ||
        !mutex_effect_pointer_representations_closed(*main->getParent()) ||
        !main->getParent()->getModuleInlineAsm().empty()) return false;
    for (auto it = ctx.bundle->global_begin(); it != ctx.bundle->global_end(); ++it)
      if ((*it)->has_frontend())
        if (auto* g = llvm::dyn_cast< llvm::GlobalVariable >((*it)->frontend< llvm::Value >())) globals.emplace(g, *it);
    for (auto it = ctx.bundle->function_begin(); it != ctx.bundle->function_end(); ++it) {
      if (!(*it)->is_definition() || !(*it)->has_frontend()) continue;
      functions.emplace((*it)->frontend< llvm::Function >(), *it);
      for (auto* b : *(*it)->body()) for (auto* s : *b) if (s->has_frontend() && isa< ar::CallBase >(s))
        if (!originals.emplace(s->frontend< llvm::Value >(), s).second) return false;
    }
    for (const auto& item : globals) {
      auto* t = llvm::dyn_cast< llvm::StructType >(item.first->getValueType());
      if (t && t->hasName() && t->getName() == "union.sem_t" && !semaphore(item.first, main)) return false;
    }
    if (effects.empty()) return false;
    for (auto& f : *main->getParent()) if (&f == main || !f.use_empty())
      for (auto& b : f) for (auto& i : b) {
      if (!step()) return false;
      auto* c = llvm::dyn_cast< llvm::CallBase >(&i);
      if (c && callee(c) && callee(c)->getName() == "malloc" && !heap(c, main)) return false;
    }
    // Closed source call set excludes user signal handlers, cancellation,
    // setxid, errno observers and callbacks outside the registered workers.
    for (auto& f : *main->getParent()) {
      if (!f.isDeclaration()) {
        if (!functions.count(&f)) return false;
        std::vector< llvm::Value* > todo{&f}; std::set< llvm::Value* > seen;
        while (!todo.empty()) {
          auto* v = todo.back(); todo.pop_back();
          if (!step()) return false;
          if (!seen.insert(v).second) continue;
          for (auto* u : v->users()) {
            if (llvm::isa< llvm::BitCastOperator >(u)) { todo.push_back(u); continue; }
            auto* c = llvm::dyn_cast< llvm::CallBase >(u);
            if (!c || !binding(c)) return false;
            if (c->getCalledOperand() == v) continue;
            if (!handle(c, true) || c->getArgOperand(2) != v || c->getFunction() != main) return false;
          }
        }
      }
      if (&f != main && f.use_empty()) continue;
      for (auto& b : f) for (auto& i : b) {
        if (!step() || llvm::isa< llvm::InvokeInst >(i) || llvm::isa< llvm::CallBrInst >(i) ||
            llvm::isa< llvm::IndirectBrInst >(i) || llvm::isa< llvm::ResumeInst >(i)) return false;
        auto* c = llvm::dyn_cast< llvm::CallBase >(&i);
        if (!c || llvm::isa< llvm::DbgInfoIntrinsic >(c)) continue;
        auto* a = binding(c); auto* target = callee(c);
        if (!a || !target) return false;
        if (!target->isDeclaration()) continue;
        if (effects.count(a) || pure(c) || allocations.count(c) || handles.count(c)) continue;
        if (handle(c, true) && (ctx.native_handle_accesses.count(a) || unread_private_thread_output(a))) continue;
        if (handle(c, false) && ctx.native_handle_joins.count(a)) continue;
        return false;
      }
    }
    return true;
  }
public:
  std::map< ar::CallBase*, ar::GlobalVariable* > effects;
  std::set< llvm::CallBase* > handles, allocations;
  std::vector< Token > tokens;
  explicit Impl(Context& context) : ctx(context) {
    if (!build()) { effects.clear(); handles.clear(); allocations.clear(); tokens.clear(); }
  }
};
PosixSemaphore::PosixSemaphore(Context& ctx) : _impl(std::make_unique< Impl >(ctx)) {}
PosixSemaphore::~PosixSemaphore() = default;
ar::GlobalVariable* PosixSemaphore::effect(ar::CallBase* c) const {
  auto i = _impl->effects.find(c); return i == _impl->effects.end() ? nullptr : i->second;
}
bool PosixSemaphore::has_effects() const { return !_impl->effects.empty(); }
bool PosixSemaphore::opaque_handle_call(llvm::CallBase* c) const { return _impl->handles.count(c); }
bool PosixSemaphore::allocation_call(llvm::CallBase* c) const { return _impl->allocations.count(c); }
const std::vector< PosixSemaphore::Token >& PosixSemaphore::tokens() const { return _impl->tokens; }
void configure_posix_semaphore(Context& ctx) {
  if (!ctx.posix_semaphore()) ctx._posix_semaphore = std::make_shared< PosixSemaphore >(ctx);
}
} // namespace analyzer
} // namespace ikos
