// Copyright and license: see analysis/context.hpp.
// First subset: scalar globals/locals, startup argv, counted i32 stack pools,
// exact pthread byte effects, and literal/integer fprintf on runtime stderr.
// Heap objects and mutexes deliberately remain outside this first subset.
#include <llvm/ADT/SmallString.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Module.h>
#include <algorithm>
#include <cstdint>
#include <ikos/analyzer/analysis/call_context.hpp>
#include <ikos/analyzer/analysis/context.hpp>
#include <ikos/analyzer/analysis/execution_engine/native_thread_handles.hpp>
#include <ikos/analyzer/analysis/execution_engine/stdio_effects.hpp>
#include <ikos/analyzer/analysis/runtime_stdio_frame.hpp>
#include <ikos/analyzer/util/log.hpp>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>

namespace ikos {
namespace analyzer {
namespace {
struct Refuse {
  std::string reason;
};
llvm::Function* source_callee(llvm::CallBase* c) {
  return llvm::dyn_cast< llvm::Function >(
      c->getCalledOperand()->stripPointerCasts());
}
bool integer(llvm::Type* t) {
  return t->isIntegerTy(32) || t->isIntegerTy(64);
}
bool null(llvm::Value* v) {
  return llvm::isa< llvm::ConstantPointerNull >(v);
}
struct Pool {
  llvm::AllocaInst* source;
  ar::Allocate* imported;
  llvm::GlobalVariable* count;
  llvm::LoadInst* snapshot;
  std::uint64_t upper;
};
} // namespace

class RuntimeStdioFrame::Impl {
  const AnalysisOptions& opts;
  ar::Bundle* const bundle;
  llvm::Function* main = nullptr;
  llvm::Module* module = nullptr;
  llvm::GlobalVariable* stderr_global = nullptr;
  std::map< llvm::Function*, ar::Function* > functions;
  std::map< llvm::Instruction*, ar::Statement* > originals;
  std::map< llvm::GlobalVariable*, ar::GlobalVariable* > globals;
  std::set< llvm::Function* > reachable, active, roots;
  std::set< llvm::Instruction* > consumed;
  std::set< llvm::CallBase* > creates, joins, scans, outputs, saves, restores;
  std::set< ar::CallBase* > output_certificates, admitted_calls;
  std::map< ar::CallBase*, ar::GlobalVariable* > scan_certificates;
  std::set< llvm::LoadInst* > input_loads;
  std::map< llvm::AllocaInst*, Pool > pools;
  std::map< llvm::Function*, std::unique_ptr< llvm::DominatorTree > >
      dominators;
  std::map< llvm::Function*, std::unique_ptr< llvm::LoopInfo > > loop_info;
  std::map< llvm::AllocaInst*, std::set< llvm::Instruction* > > storage_uses;
  std::string refusal;
  unsigned budget = 65536;
  bool valid = false;

  void need(bool yes, const char* why) const {
    if (!yes)
      throw Refuse{why};
  }
  void step() {
    need(budget != 0, "source/AR frame budget exhausted");
    --budget;
  }
  ar::Statement* original(llvm::Instruction* i) const {
    auto p = originals.find(i);
    return p == originals.end() ? nullptr : p->second;
  }
  llvm::DominatorTree& dom(llvm::Function* f) {
    auto& p = dominators[f];
    if (!p)
      p.reset(new llvm::DominatorTree(*f));
    return *p;
  }
  llvm::LoopInfo& loops(llvm::Function* f) {
    auto& p = loop_info[f];
    if (!p)
      p.reset(new llvm::LoopInfo(dom(f)));
    return *p;
  }
  bool after(llvm::Instruction* from, llvm::Instruction* to) {
    need(from->getFunction() == to->getFunction(),
         "cross-function order query");
    for (auto* n = from->getNextNode(); n; n = n->getNextNode())
      if (n == to)
        return true;
    std::vector< llvm::BasicBlock* > work(llvm::succ_begin(from->getParent()),
                                          llvm::succ_end(from->getParent()));
    std::set< llvm::BasicBlock* > seen;
    while (!work.empty()) {
      step();
      auto* b = work.back();
      work.pop_back();
      if (b == to->getParent())
        return true;
      if (!seen.insert(b).second)
        continue;
      work.insert(work.end(), llvm::succ_begin(b), llvm::succ_end(b));
    }
    return false;
  }
  bool ar_after(ar::Statement* from, ar::Statement* to) {
    if (!from || !to || from->code() != to->code())
      return true;
    bool passed = false;
    for (auto* s : *from->parent()) {
      if (passed && s == to)
        return true;
      if (s == from)
        passed = true;
    }
    std::vector< ar::BasicBlock* > work(from->parent()->successor_begin(),
                                        from->parent()->successor_end());
    std::set< ar::BasicBlock* > seen;
    while (!work.empty()) {
      step();
      auto* b = work.back();
      work.pop_back();
      if (b == to->parent())
        return true;
      if (!seen.insert(b).second)
        continue;
      work.insert(work.end(), b->successor_begin(), b->successor_end());
    }
    return false;
  }
  bool acyclic(llvm::Instruction* i) { return !after(i, i); }
  bool startup(llvm::Instruction* i) {
    if (i->getFunction() != main)
      return false;
    for (auto* c : creates)
      if (after(c, i) || ar_after(original(c), original(i)))
        return false;
    return true;
  }
  ar::Value* peel(ar::Value* v) const {
    for (unsigned n = 0; n < 32; ++n) {
      auto* p = dyn_cast< ar::InternalVariable >(v);
      auto* d = p ? unique_def(p) : nullptr;
      if (auto* a = dyn_cast_or_null< ar::Assignment >(d))
        v = a->operand();
      else if (auto* c = dyn_cast_or_null< ar::UnaryOperation >(d)) {
        if (c->op() != ar::UnaryOperation::Bitcast)
          return v;
        v = c->operand();
      } else if (auto* s = dyn_cast_or_null< ar::PointerShift >(d)) {
        bool zero = true;
        for (auto it = s->term_begin(); it != s->term_end(); ++it) {
          auto* k = dyn_cast< ar::IntegerConstant >((*it).second);
          zero &= k && k->value().is_zero();
        }
        if (!zero)
          return v;
        v = s->pointer();
      } else
        return v;
    }
    return nullptr;
  }
  bool value(ar::Value* a, llvm::Value* n) const {
    if (!a || !n)
      return false;
    if (auto* k = llvm::dyn_cast< llvm::ConstantInt >(n)) {
      auto* x = dyn_cast< ar::IntegerConstant >(a);
      if (!x || x->value().bit_width() != k->getBitWidth())
        return false;
      llvm::SmallString< 32 > text;
      k->getValue().toString(text,
                             10,
                             cast< ar::IntegerType >(x->type())->is_signed());
      return x->value().to_z_number() ==
             core::ZNumber::from_string(text.str().str());
    }
    if (null(n))
      return isa< ar::NullConstant >(a);
    auto* stripped = n->getType()->isPointerTy() ? n->stripPointerCasts() : n;
    if (auto* f = llvm::dyn_cast< llvm::Function >(stripped)) {
      auto* x = dyn_cast< ar::FunctionPointerConstant >(peel(a));
      return x && x->function()->has_frontend() &&
             x->function()->frontend< llvm::Value >() == f;
    }
    for (unsigned depth = 0; depth < 2; ++depth) {
      if (auto* v = dyn_cast< ar::Variable >(a))
        if (v->has_frontend() && (v->frontend< llvm::Value >() == n ||
                                  v->frontend< llvm::Value >() == stripped))
          return true;
      if (auto* i = llvm::dyn_cast< llvm::Instruction >(n)) {
        auto* s = original(i);
        if (s && s->result_or_null() == a)
          return true;
      }
      if (auto* arg = llvm::dyn_cast< llvm::Argument >(n)) {
        auto f = functions.find(arg->getParent());
        if (f != functions.end() && f->second->param(arg->getArgNo()) == a)
          return true;
      }
      a = peel(a);
      if (!a)
        return false;
    }
    return false;
  }
  ar::Intrinsic::ID mapped(llvm::Function* f) const {
    const auto n = f->getName();
    if (n == "fprintf")
      return ar::Intrinsic::LibcFprintf;
    if (n == "sscanf" || n == "__isoc99_sscanf")
      return ar::Intrinsic::LibcSscanf;
    if (n == "abort")
      return ar::Intrinsic::LibcAbort;
    if (n == "exit")
      return ar::Intrinsic::LibcExit;
    if (n == "llvm.stacksave")
      return ar::Intrinsic::StackSave;
    if (n == "llvm.stackrestore")
      return ar::Intrinsic::StackRestore;
    if (n == "__ikos_nondet_int" || n == "__ikos_nondet_int64")
      return ar::Intrinsic::IkosNonDet;
    if (n == "pthread_create")
      return ar::Intrinsic::PthreadCreate;
    if (n == "pthread_join")
      return ar::Intrinsic::PthreadJoin;
    return ar::Intrinsic::NotIntrinsic;
  }
  ar::CallBase* binding(llvm::CallBase* c) const {
    auto* a = dyn_cast_or_null< ar::Call >(original(c));
    auto* f = source_callee(c);
    auto* t = a ? native_handle_callee(a) : nullptr;
    if (!a || !f || !t || !llvm::isa< llvm::CallInst >(c) || c->isInlineAsm() ||
        c->hasOperandBundles() || c->getCallingConv() != llvm::CallingConv::C ||
        f->getCallingConv() != llvm::CallingConv::C ||
        c->getFunctionType() != f->getFunctionType() || c->arg_size() > 64 ||
        c->arg_size() != a->num_arguments() ||
        f->arg_size() != t->num_parameters() ||
        f->isVarArg() != t->type()->is_var_arg() ||
        (f->isDeclaration() ? !t->is_declaration() : !t->is_definition()))
      return nullptr;
    if (t->has_frontend()) {
      if (t->frontend< llvm::Value >() != f)
        return nullptr;
    } else if (!f->isDeclaration() ||
               mapped(f) == ar::Intrinsic::NotIntrinsic ||
               t->intrinsic_id() != mapped(f))
      return nullptr;
    for (unsigned i = 0; i < c->arg_size(); ++i) {
      if (!value(a->argument(i), c->getArgOperand(i)))
        return nullptr;
      if (i < f->arg_size() &&
          a->argument(i)->type() != t->type()->param_type(i))
        return nullptr;
      for (auto attr : {llvm::Attribute::ByVal,
                        llvm::Attribute::StructRet,
                        llvm::Attribute::InAlloca,
                        llvm::Attribute::Preallocated})
        if (c->paramHasAttr(i, attr) ||
            (i < f->arg_size() && f->hasParamAttribute(i, attr)))
          return nullptr;
    }
    return a;
  }
  std::string literal(ar::Value* a, llvm::Value* n) {
    auto* g = llvm::dyn_cast< llvm::GlobalVariable >(n->stripPointerCasts());
    auto* ag = dyn_cast_or_null< ar::GlobalVariable >(peel(a));
    need(g && ag && globals.count(g) && globals.at(g) == ag &&
             g->isConstant() && g->hasInitializer() &&
             !g->isExternallyInitialized() && g->getAddressSpace() == 0,
         "literal source/AR identity");
    auto* bytes =
        llvm::dyn_cast< llvm::ConstantDataArray >(g->getInitializer());
    need(bytes && bytes->getElementType()->isIntegerTy(8) &&
             bytes->isCString() && bytes->getNumElements() <= 4096,
         "literal byte array");
    auto* ar_array = dyn_cast< ar::ArrayType >(ag->type()->pointee());
    auto* ar_byte = ar_array
                        ? dyn_cast< ar::IntegerType >(ar_array->element_type())
                        : nullptr;
    need(ar_array && ar_byte && ar_byte->bit_width() == 8 &&
             ar_array->num_elements() == core::ZNumber(bytes->getNumElements()),
         "literal global byte extent");
    unsigned stores = 0;
    for (auto* b : *ag->initializer())
      for (auto* s : *b) {
        step();
        auto* store = dyn_cast< ar::Store >(s);
        auto* array =
            store ? dyn_cast< ar::ArrayConstant >(store->value()) : nullptr;
        need(store && store->pointer() == ag && array &&
                 array->values().size() == bytes->getNumElements(),
             "literal AR initializer");
        for (unsigned i = 0; i < bytes->getNumElements(); ++i) {
          step();
          auto* k = dyn_cast< ar::IntegerConstant >(array->values()[i]);
          need(k && k->value().bit_width() == 8 &&
                   bytes->getElementAsInteger(i) < 128 &&
                   k->value().to_z_number() ==
                       core::ZNumber(bytes->getElementAsInteger(i)),
               "literal contents mismatch");
        }
        ++stores;
      }
    need(stores == 1, "literal complete initializer");
    return bytes->getAsCString().str();
  }
  void scalar_global(llvm::GlobalVariable* g) {
    need(g && globals.count(g) && integer(g->getValueType()) &&
             g->hasInitializer() && !g->isConstant() && !g->isThreadLocal() &&
             !g->isExternallyInitialized() && g->getAddressSpace() == 0,
         "whole scalar global storage");
    auto* ag = globals.at(g);
    auto* at = dyn_cast< ar::IntegerType >(ag->type()->pointee());
    need(ag->is_definition() && !ag->is_thread_local() && at &&
             at->bit_width() == g->getValueType()->getIntegerBitWidth(),
         "scalar global AR type");
    auto* native = llvm::dyn_cast< llvm::ConstantInt >(g->getInitializer());
    need(native, "scalar global constant initializer");
    unsigned n = 0;
    for (auto* b : *ag->initializer())
      for (auto* s : *b) {
        auto* st = dyn_cast< ar::Store >(s);
        need(st && st->pointer() == ag && value(st->value(), native),
             "scalar global AR initializer");
        ++n;
      }
    need(n == 1, "scalar global complete initializer");
  }
  void stderr_storage() {
    stderr_global = module->getNamedGlobal("stderr");
    auto* p =
        stderr_global
            ? llvm::dyn_cast< llvm::PointerType >(stderr_global->getValueType())
            : nullptr;
    auto* file =
        p && !p->isOpaque()
            ? llvm::dyn_cast< llvm::StructType >(p->getPointerElementType())
            : nullptr;
    need(stderr_global && stderr_global->isDeclaration() &&
             !stderr_global->isConstant() && !stderr_global->isThreadLocal() &&
             !stderr_global->isExternallyInitialized() &&
             stderr_global->getAddressSpace() == 0 && p &&
             p->getAddressSpace() == 0 && file && file->hasName() &&
             file->getName() == "struct._IO_FILE" &&
             globals.count(stderr_global),
         "actual runtime stderr declaration");
    auto* ag = globals.at(stderr_global);
    need(!ag->is_definition() && !ag->is_thread_local() &&
             ag->type()->pointee()->is_pointer() &&
             bundle->data_layout().store_size_in_bytes(ag->type()->pointee()) ==
                 4,
         "stderr AR declaration");
  }
  void fprintf_call(llvm::CallBase* c, ar::CallBase* a) {
    auto* f = source_callee(c);
    auto* t = native_handle_callee(a);
    need(f->isDeclaration() && f->getName() == "fprintf" && f->isVarArg() &&
             f->arg_size() == 2 && c->arg_size() >= 2 &&
             c->getType()->isIntegerTy(32) &&
             t->intrinsic_id() == ar::Intrinsic::LibcFprintf,
         "exact fprintf ABI");
    auto* native = llvm::dyn_cast< llvm::LoadInst >(
        c->getArgOperand(0)->stripPointerCasts());
    auto* load =
        native ? dyn_cast_or_null< ar::Load >(original(native)) : nullptr;
    need(native && load &&
             native->getPointerOperand()->stripPointerCasts() ==
                 stderr_global &&
             native->getType() == stderr_global->getValueType() &&
             !native->isVolatile() && !native->isAtomic() &&
             value(a->argument(0), native) &&
             value(load->operand(), stderr_global) &&
             load->result()->type()->is_pointer(),
         "actual stderr load identity");
    const auto format = literal(a->argument(1), c->getArgOperand(1));
    unsigned conversions = 0;
    for (std::size_t i = 0; i < format.size(); ++i) {
      if (format[i] != '%')
        continue;
      need(++i < format.size(), "unfinished output conversion");
      if (format[i] == '%')
        continue;
      need(format[i] == 'd', "unsupported output conversion");
      ++conversions;
    }
    need(c->arg_size() == conversions + 2, "output argument count");
    for (unsigned i = 2; i < c->arg_size(); ++i) {
      auto* ty = dyn_cast< ar::IntegerType >(a->argument(i)->type());
      need(c->getArgOperand(i)->getType()->isIntegerTy(32) && ty &&
               ty->bit_width() == 32,
           "promoted output integer width");
    }
    outputs.insert(c);
  }
  bool ar_dominates(ar::Statement* proof, ar::Statement* use) {
    if (proof->code() != use->code())
      return false;
    if (proof->parent() == use->parent()) {
      for (auto* s : *proof->parent()) {
        if (s == proof)
          return true;
        if (s == use)
          return false;
      }
      return false;
    }
    std::vector< ar::BasicBlock* > work{proof->code()->entry_block()};
    std::set< ar::BasicBlock* > seen;
    while (!work.empty()) {
      step();
      auto* b = work.back();
      work.pop_back();
      if (b == proof->parent() || !seen.insert(b).second)
        continue;
      if (b == use->parent())
        return false;
      work.insert(work.end(), b->successor_begin(), b->successor_end());
    }
    return true;
  }
  ar::Comparison* ar_guard(llvm::ICmpInst* cmp,
                           llvm::Value* left,
                           llvm::Value* right,
                           ar::Comparison::Predicate pred,
                           ar::Statement* use) {
    for (auto* b : *functions.at(cmp->getFunction())->body())
      for (auto* s : *b) {
        step();
        auto* c = dyn_cast< ar::Comparison >(s);
        if (c && c->has_frontend() && c->frontend< llvm::Value >() == cmp &&
            c->predicate() == pred && value(c->left(), left) &&
            value(c->right(), right) && ar_dominates(c, use))
          return c;
      }
    return nullptr;
  }
  void pthread_call(llvm::CallBase* c, ar::CallBase* a, bool create) {
    auto* f = source_callee(c);
    const unsigned n = create ? 4 : 2;
    need(c->getFunction() == main && f->isDeclaration() && !f->isVarArg() &&
             c->arg_size() == n && f->arg_size() == n &&
             c->getType()->isIntegerTy(32) &&
             f->getReturnType()->isIntegerTy(32),
         "pthread effect ABI");
    auto* rt = dyn_cast< ar::IntegerType >(
        native_handle_callee(a)->type()->return_type());
    need(rt && rt->bit_width() == 32, "pthread AR result ABI");
    for (unsigned i = 0; i < n; ++i) {
      if (!create && i == 0) {
        need(c->getArgOperand(i)->getType()->isIntegerTy(32) &&
                 a->argument(i)->type()->is_integer() &&
                 cast< ar::IntegerType >(a->argument(i)->type())->bit_width() ==
                     32,
             "join handle ABI");
      } else {
        auto* p =
            llvm::dyn_cast< llvm::PointerType >(c->getArgOperand(i)->getType());
        need(p && p->getAddressSpace() == 0 &&
                 a->argument(i)->type()->is_pointer(),
             "pthread pointer ABI");
      }
    }
    need(null(c->getArgOperand(1)) && isa< ar::NullConstant >(a->argument(1)),
         "pthread attributes/output must be literal NULL");
    if (create) {
      auto* p = llvm::cast< llvm::PointerType >(c->getArgOperand(0)->getType());
      auto* ap = cast< ar::PointerType >(a->argument(0)->type());
      auto* handle = dyn_cast< ar::IntegerType >(ap->pointee());
      need(handle && handle->bit_width() == 32,
           "actual AR create output width");
      auto* child = llvm::dyn_cast< llvm::Function >(
          c->getArgOperand(2)->stripPointerCasts());
      need(!p->isOpaque() && p->getPointerElementType()->isIntegerTy(32) &&
               child && child != main && !child->isDeclaration() &&
               child->arg_size() == 1 &&
               child->getArg(0)->getType()->isPointerTy() &&
               child->getArg(0)->use_empty() &&
               child->getReturnType()->isPointerTy() &&
               null(c->getArgOperand(3)) &&
               isa< ar::NullConstant >(a->argument(3)),
           "closed child/output shape");
      auto* signature =
          llvm::cast< llvm::PointerType >(c->getArgOperand(2)->getType());
      need(!signature->isOpaque() &&
               signature->getPointerElementType() == child->getFunctionType() &&
               llvm::cast< llvm::PointerType >(child->getArg(0)->getType())
                       ->getAddressSpace() == 0 &&
               llvm::cast< llvm::PointerType >(child->getReturnType())
                       ->getAddressSpace() == 0,
           "actual child function-pointer ABI");
      creates.insert(c);
      roots.insert(child);
      discover(child);
    } else {
      auto* p = llvm::cast< llvm::PointerType >(c->getArgOperand(1)->getType());
      need(!p->isOpaque() && p->getPointerElementType()->isPointerTy() &&
               llvm::isa< llvm::LoadInst >(c->getArgOperand(0)),
           "join actual scalar load");
      joins.insert(c);
    }
    // Deliberately no use_empty check: this is only a byte-effect descriptor.
    // It writes no native-handle registry and conveys no success, value or HB.
  }
  void discover(llvm::Function* f) {
    need(!active.count(f), "recursive reachable helper");
    if (reachable.count(f))
      return;
    need(functions.count(f) && !f->isDeclaration() && !f->isVarArg() &&
             f->getCallingConv() == llvm::CallingConv::C,
         "defined reachable function binding");
    active.insert(f);
    reachable.insert(f);
    for (auto& argument : f->args()) {
      need(f == main || integer(argument.getType()) ||
               (roots.count(f) && argument.getType()->isPointerTy() &&
                argument.use_empty()),
           "pointer-taking helper or child argument use");
      for (auto attr : {llvm::Attribute::ByVal,
                        llvm::Attribute::StructRet,
                        llvm::Attribute::InAlloca,
                        llvm::Attribute::Preallocated})
        need(!f->hasParamAttribute(argument.getArgNo(), attr),
             "hidden helper ABI effect");
    }
    for (auto& b : *f)
      for (auto& i : b) {
        step();
        auto* c = llvm::dyn_cast< llvm::CallBase >(&i);
        if (!c || llvm::isa< llvm::DbgInfoIntrinsic >(c))
          continue;
        auto* a = binding(c);
        auto* callee = source_callee(c);
        need(a && callee, "complete actual call binding");
        consumed.insert(c);
        admitted_calls.insert(a);
        if (!callee->isDeclaration()) {
          need(callee != main && !roots.count(callee) &&
                   (callee->getReturnType()->isVoidTy() ||
                    integer(callee->getReturnType())),
               "unsupported internal pointer/root call");
          for (auto& arg : c->args())
            need(integer(arg->getType()), "pointer helper actual");
          discover(callee);
          continue;
        }
        const auto name = callee->getName();
        if (name == "pthread_create" || name == "pthread_join") {
          pthread_call(c, a, name == "pthread_create");
        } else if (name == "fprintf") {
          fprintf_call(c, a);
        } else if (name == "sscanf" || name == "__isoc99_sscanf") {
          auto effect = scalar_scan_effect(a);
          need(effect && effect->implicit_state_closed && f == main,
               "complete startup scalar-scan descriptor");
          scans.insert(c);
        } else if ((name == "__VERIFIER_atomic_begin" ||
                    name == "__VERIFIER_atomic_end") &&
                   c->arg_empty() && c->getType()->isVoidTy() &&
                   !callee->isVarArg()) {
          // Existing SV-COMP pseudo-atomic API; no stream/application byte
          // effect.
        } else if ((name == "__ikos_nondet_int" ||
                    name == "__ikos_nondet_int64") &&
                   c->arg_empty() && c->getMetadata("ikos.freeze_uninit") &&
                   native_handle_callee(a)->intrinsic_id() ==
                       ar::Intrinsic::IkosNonDet &&
                   c->getType()->isIntegerTy(
                       name == "__ikos_nondet_int" ? 32 : 64)) {
          // Actual frontend nondet initializer, not a memory-effect exemption.
        } else if ((name == "abort" && c->arg_empty() &&
                    c->getType()->isVoidTy() &&
                    native_handle_callee(a)->intrinsic_id() ==
                        ar::Intrinsic::LibcAbort) ||
                   (name == "exit" && c->arg_size() == 1 &&
                    c->getType()->isVoidTy() &&
                    c->getArgOperand(0)->getType()->isIntegerTy(32) &&
                    native_handle_callee(a)->intrinsic_id() ==
                        ar::Intrinsic::LibcExit) ||
                   constant_assertion_effect(a)) {
          // Terminal effects. No callback/atexit/signal registration is
          // admitted.
        } else if (name == "llvm.stacksave" && f == main && c->arg_empty() &&
                   c->getType()->isPointerTy() &&
                   native_handle_callee(a)->intrinsic_id() ==
                       ar::Intrinsic::StackSave) {
          saves.insert(c);
        } else if (name == "llvm.stackrestore" && f == main &&
                   c->arg_size() == 1 && c->getType()->isVoidTy() &&
                   native_handle_callee(a)->intrinsic_id() ==
                       ar::Intrinsic::StackRestore) {
          restores.insert(c);
        } else {
          throw Refuse{
              "reachable external effect outside the first stdio frame"};
        }
      }
    active.erase(f);
  }
  llvm::LoadInst* count_load(llvm::Value* v) const {
    auto* l = llvm::dyn_cast< llvm::LoadInst >(v);
    auto* g = l ? llvm::dyn_cast< llvm::GlobalVariable >(
                      l->getPointerOperand()->stripPointerCasts())
                : nullptr;
    return l && g && l->getFunction() == main && !l->isAtomic() &&
                   !l->isVolatile() && l->getType()->isIntegerTy(32) &&
                   g->getValueType()->isIntegerTy(32)
               ? l
               : nullptr;
  }
  llvm::GlobalVariable* count_global(llvm::Value* v) const {
    auto* l = count_load(v);
    return l ? llvm::cast< llvm::GlobalVariable >(
                   l->getPointerOperand()->stripPointerCasts())
             : nullptr;
  }
  bool stable_after(llvm::GlobalVariable* g, llvm::Instruction* snapshot) {
    for (auto* f : reachable)
      for (auto& b : *f)
        for (auto& i : b) {
          step();
          bool writer = false;
          if (auto* s = llvm::dyn_cast< llvm::StoreInst >(&i))
            writer = s->getPointerOperand()->stripPointerCasts() == g;
          if (auto* c = llvm::dyn_cast< llvm::CallBase >(&i))
            writer |=
                scans.count(c) && c->getArgOperand(2)->stripPointerCasts() == g;
          if (writer && (f != main || after(snapshot, &i) ||
                         ar_after(original(snapshot), original(&i))))
            return false;
        }
    // Every other writer is independently bounded/disjoint before sealing.
    return true;
  }
  std::uint64_t upper_bound(llvm::AllocaInst* storage,
                            ar::Allocate* a,
                            llvm::GlobalVariable* g) {
    for (auto& b : *main) {
      step();
      auto* branch = llvm::dyn_cast< llvm::BranchInst >(b.getTerminator());
      auto* cmp = branch && branch->isConditional()
                      ? llvm::dyn_cast< llvm::ICmpInst >(branch->getCondition())
                      : nullptr;
      if (!cmp || count_global(cmp->getOperand(0)) != g)
        continue;
      auto* bound = llvm::dyn_cast< llvm::ConstantInt >(cmp->getOperand(1));
      if (!bound || bound->getBitWidth() != 32)
        continue;
      auto n = bound->getSExtValue();
      if (n < 1 || n > INT32_MAX / 4)
        continue;
      unsigned edge = 0;
      if (cmp->getPredicate() == llvm::CmpInst::ICMP_SGT)
        edge = 1;
      else if (cmp->getPredicate() != llvm::CmpInst::ICMP_SLE)
        continue;
      if (!dom(main).dominates(llvm::BasicBlockEdge(&b,
                                                    branch->getSuccessor(edge)),
                               storage->getParent()) ||
          !stable_after(g, count_load(cmp->getOperand(0))) ||
          !ar_guard(cmp, cmp->getOperand(0), bound, ar::Comparison::SILE, a))
        continue;
      return static_cast< std::uint64_t >(n);
    }
    throw Refuse{"no stable native/AR upper-bound edge for VLA"};
  }
  void allocations() {
    for (auto* f : reachable)
      for (auto& b : *f)
        for (auto& i : b) {
          auto* storage = llvm::dyn_cast< llvm::AllocaInst >(&i);
          if (!storage)
            continue;
          step();
          auto* a = dyn_cast_or_null< ar::Allocate >(original(storage));
          auto* at =
              a ? dyn_cast< ar::IntegerType >(a->allocated_type()) : nullptr;
          need(a && at && integer(storage->getAllocatedType()) &&
                   storage->getAddressSpace() == 0 &&
                   at->bit_width() ==
                       storage->getAllocatedType()->getIntegerBitWidth() &&
                   value(a->result(), storage) &&
                   value(a->array_size(), storage->getArraySize()) &&
                   acyclic(storage),
               "whole live scalar allocation identity");
          consumed.insert(storage);
          if (auto* constant = llvm::dyn_cast< llvm::ConstantInt >(
                  storage->getArraySize())) {
            need(constant->isOne(), "fixed arrays outside first frame");
            continue;
          }
          auto* load = count_load(storage->getArraySize());
          auto* g = count_global(storage->getArraySize());
          need(f == main && storage->getAllocatedType()->isIntegerTy(32) &&
                   load && g && storage->getAlign().value() >= 4 &&
                   startup(storage),
               "startup i32 VLA source");
          scalar_global(g);
          need(stable_after(g, load), "VLA count changed after snapshot");
          auto upper = upper_bound(storage, a, g);
          pools.emplace(storage, Pool{storage, a, g, load, upper});
        }
  }
  bool repeats_without_header(llvm::Instruction* site,
                              llvm::BasicBlock* header) {
    std::vector< llvm::BasicBlock* > work(llvm::succ_begin(site->getParent()),
                                          llvm::succ_end(site->getParent()));
    std::set< llvm::BasicBlock* > seen;
    while (!work.empty()) {
      step();
      auto* block = work.back();
      work.pop_back();
      if (block == header || !seen.insert(block).second)
        continue;
      if (block == site->getParent())
        return true;
      work.insert(work.end(), llvm::succ_begin(block), llvm::succ_end(block));
    }
    return false;
  }
  bool ar_repeats_without(ar::Statement* site, ar::Statement* commit) {
    // A backedge PHI assignment commits the actual increment. Cutting only
    // the add would allow an irreducible cycle to reuse the old PHI value.
    bool passed = false;
    for (auto* statement : *site->parent()) {
      if (passed && statement == commit)
        return false;
      if (statement == site)
        passed = true;
    }
    std::vector< ar::BasicBlock* > work(site->parent()->successor_begin(),
                                        site->parent()->successor_end());
    std::set< ar::BasicBlock* > seen;
    while (!work.empty()) {
      step();
      auto* block = work.back();
      work.pop_back();
      if (!seen.insert(block).second)
        continue;
      bool cut = false;
      for (auto* statement : *block) {
        step();
        if (statement == commit) {
          cut = true;
          break;
        }
        if (statement == site)
          return true;
      }
      if (!cut)
        work.insert(work.end(),
                    block->successor_begin(),
                    block->successor_end());
    }
    return false;
  }
  ar::Assignment* ar_phi(ar::Value* index,
                         llvm::PHINode* phi,
                         llvm::BinaryOperator* update) {
    auto* variable = dyn_cast_or_null< ar::InternalVariable >(peel(index));
    need(variable && variable->has_frontend() &&
             variable->frontend< llvm::Value >() == phi &&
             variable->type()->is_integer() &&
             cast< ar::IntegerType >(variable->type())->bit_width() == 32,
         "actual AR induction variable");
    unsigned zero = 0, increment = 0, additions = 0;
    ar::Assignment* commit = nullptr;
    for (auto* b : *functions.at(main)->body())
      for (auto* s : *b) {
        step();
        if (s->result_or_null() == variable) {
          auto* copy = dyn_cast< ar::Assignment >(s);
          need(copy && copy->has_frontend() &&
                   copy->frontend< llvm::Value >() == phi,
               "unexpected imported induction definition");
          auto* k = dyn_cast< ar::IntegerConstant >(copy->operand());
          if (k && k->value().bit_width() == 32 && k->value().is_zero()) {
            need(!ar_after(copy, copy),
                 "induction zero initialization repeats");
            ++zero;
          } else {
            need(value(copy->operand(), update), "imported induction backedge");
            ++increment;
            commit = copy;
          }
        }
        if (s->has_frontend() && s->frontend< llvm::Value >() == update) {
          auto* add = dyn_cast< ar::BinaryOperation >(s);
          need(add && add->op() == ar::BinaryOperation::SAdd &&
                   value(add->left(), phi) &&
                   value(add->right(), update->getOperand(1)),
               "actual imported unit increment");
          ++additions;
        }
      }
    need(zero == 1 && increment == 1 && additions == 1,
         "complete imported induction mapping");
    return commit;
  }
  void indexed_range(llvm::Instruction* access,
                     llvm::Value* address,
                     ar::Value* ap,
                     ar::Statement* a) {
    auto* gep =
        llvm::dyn_cast< llvm::GetElementPtrInst >(address->stripPointerCasts());
    auto* base = gep ? llvm::dyn_cast< llvm::AllocaInst >(
                           gep->getPointerOperand()->stripPointerCasts())
                     : nullptr;
    need(gep && base && pools.count(base) && access->getFunction() == main &&
             gep->getNumIndices() == 1 &&
             gep->getSourceElementType()->isIntegerTy(32) &&
             dom(main).dominates(base, access),
         "native VLA access identity");
    const auto& p = pools.at(base);
    auto* phi = llvm::dyn_cast< llvm::PHINode >(gep->getOperand(1));
    auto* loop = phi ? loops(main).getLoopFor(phi->getParent()) : nullptr;
    auto* latch = loop ? loop->getLoopLatch() : nullptr;
    auto* pre = loop ? loop->getLoopPreheader() : nullptr;
    need(phi && phi->getType()->isIntegerTy(32) &&
             phi->getNumIncomingValues() == 2 && loop &&
             loop->getHeader() == phi->getParent() && !loop->getParentLoop() &&
             loop->getSubLoops().empty() && latch && pre &&
             loop->contains(access),
         "counted VLA loop shape");
    auto* start =
        llvm::dyn_cast< llvm::ConstantInt >(phi->getIncomingValueForBlock(pre));
    auto* update = llvm::dyn_cast< llvm::BinaryOperator >(
        phi->getIncomingValueForBlock(latch));
    auto* one = update
                    ? llvm::dyn_cast< llvm::ConstantInt >(update->getOperand(1))
                    : nullptr;
    auto* branch =
        llvm::dyn_cast< llvm::BranchInst >(loop->getHeader()->getTerminator());
    auto* cmp = branch && branch->isConditional()
                    ? llvm::dyn_cast< llvm::ICmpInst >(branch->getCondition())
                    : nullptr;
    need(start && start->isZero() && update &&
             update->getOpcode() == llvm::Instruction::Add &&
             update->getOperand(0) == phi && one && one->isOne() && cmp &&
             cmp->getPredicate() == llvm::CmpInst::ICMP_SLT &&
             cmp->getOperand(0) == phi &&
             count_global(cmp->getOperand(1)) == p.count &&
             loop->contains(branch->getSuccessor(0)) &&
             !loop->contains(branch->getSuccessor(1)),
         "zero/unit/signed-count loop");
    need(acyclic(pre->getTerminator()),
         "counted phase can restart at index zero");
    llvm::BasicBlockEdge body(loop->getHeader(), branch->getSuccessor(0));
    need(dom(main).dominates(body, access->getParent()) &&
             dom(main).dominates(body, update->getParent()) &&
             dom(main).dominates(update, latch->getTerminator()) &&
             stable_after(p.count, count_load(cmp->getOperand(1))),
         "count guard/update dominance");
    auto* v = dyn_cast_or_null< ar::InternalVariable >(peel(ap));
    auto* shift =
        v ? dyn_cast_or_null< ar::PointerShift >(unique_def(v)) : nullptr;
    need(shift && value(shift->pointer(), base), "actual AR VLA base");
    unsigned terms = 0;
    ar::Value* index = nullptr;
    for (auto it = shift->term_begin(); it != shift->term_end(); ++it) {
      auto* k = dyn_cast< ar::IntegerConstant >((*it).second);
      if (k && k->value().is_zero())
        continue;
      need((*it).first.to_z_number() == 4 && value((*it).second, phi),
           "actual AR VLA index/stride");
      ++terms;
      index = (*it).second;
    }
    need(terms == 1, "one complete VLA byte term");
    auto* commit = ar_phi(index, phi, update);
    need(!repeats_without_header(access, loop->getHeader()) &&
             !ar_repeats_without(a, commit),
         "one dynamic occurrence per committed index");
    auto* guard =
        ar_guard(cmp, phi, cmp->getOperand(1), ar::Comparison::SILT, a);
    auto* add = imported_pure(update);
    need(guard &&
             ar_guard(cmp,
                      phi,
                      cmp->getOperand(1),
                      ar::Comparison::SILT,
                      add) == guard &&
             ar_dominates(add, commit) && !ar_repeats_without(a, guard) &&
             !ar_repeats_without(add, guard) &&
             !ar_repeats_without(commit, add),
         "AR guarded increment and fresh PHI commit on every iteration");
    // The native +1 and imported +1 have the same induction. Under i<count<=U,
    // 0<=i, i+1<=U and 4*i+4<=4*count<=4*U<=INT_MAX. No nsw/inbounds premise.
    storage_uses[base].insert(access);
  }
  void seed_range(llvm::StoreInst* store,
                  ar::Store* a,
                  llvm::AllocaInst* base) {
    const auto& p = pools.at(base);
    auto* source = llvm::dyn_cast< llvm::CallBase >(store->getValueOperand());
    need(store->getMetadata("ikos.freeze_uninit") && source &&
             source->getMetadata("ikos.freeze_uninit") &&
             source->getType()->isIntegerTy(32) && source->arg_empty() &&
             source_callee(source) &&
             source_callee(source)->getName() == "__ikos_nondet_int" &&
             binding(source) && value(a->pointer(), base) &&
             value(a->value(), source) && dom(main).dominates(base, store),
         "actual first-cell seed shape");
    bool guarded = false;
    for (auto& b : *main) {
      step();
      auto* br = llvm::dyn_cast< llvm::BranchInst >(b.getTerminator());
      auto* cmp = br && br->isConditional()
                      ? llvm::dyn_cast< llvm::ICmpInst >(br->getCondition())
                      : nullptr;
      if (!cmp || cmp->getPredicate() != llvm::CmpInst::ICMP_NE ||
          cmp->getOperand(0) != base->getArraySize())
        continue;
      auto* zero = llvm::dyn_cast< llvm::ConstantInt >(cmp->getOperand(1));
      if (!zero || !zero->isZero())
        continue;
      if (!dom(main).dominates(llvm::BasicBlockEdge(&b, br->getSuccessor(0)),
                               store->getParent()))
        continue;
      guarded |=
          ar_guard(cmp, base->getArraySize(), zero, ar::Comparison::UINE, a) ||
          ar_guard(cmp, base->getArraySize(), zero, ar::Comparison::SINE, a);
    }
    need(guarded && startup(store),
         "physical seed lacks matching native/AR nonzero guard");
    // Existing Allocate contract, not a new source/path assumption:
    // the admitted interprocedural value engine enables UpdateAllocSizeVar and
    // uses unsigned MulNoWrap for this exact AR Allocate (see draft notes).
    // On its normal continuation extent=4*u32(count), without wrap. The same
    // count!=0 guard gives extent>=4. Negative signed counts are NOT declared
    // positive; their explicit signed create/read loops are never enabled.
    need(p.imported->array_size()->type()->is_integer() &&
             cast< ar::IntegerType >(p.imported->array_size()->type())
                     ->bit_width() == 32 &&
             !cast< ar::IntegerType >(p.imported->array_size()->type())
                  ->is_signed(),
         "seed requires actual unsigned Allocate size contract");
    storage_uses[base].insert(store);
  }
  void argv_read(llvm::LoadInst* load, ar::Load* a) {
    auto* gep = llvm::dyn_cast< llvm::GetElementPtrInst >(
        load->getPointerOperand()->stripPointerCasts());
    need(gep && gep->getNumIndices() == 1 && main->arg_size() == 2 &&
             gep->getPointerOperand() == main->getArg(1) &&
             gep->getSourceElementType()->isPointerTy() &&
             load->getType()->isPointerTy() &&
             a->result()->type()->is_pointer() && startup(load),
         "startup argv pointer load");
    auto* k = llvm::dyn_cast< llvm::ConstantInt >(gep->getOperand(1));
    need(k && k->getBitWidth() == 32 && k->getSExtValue() >= 0 &&
             k->getSExtValue() <= 16 && value(a->operand(), gep),
         "bounded argv index identity");
    // These input reads stay explicit unknown-length Read effects. They are
    // allowed by serial phase, not by asserting argv/FILE disjointness.
    for (auto* use : load->users()) {
      auto* c = llvm::dyn_cast< llvm::CallBase >(use);
      need(c && scans.count(c) && c->getArgOperand(0) == load && startup(c),
           "argv string escapes startup scan input");
    }
    need(!load->user_empty(), "unconsumed foreign pointer load");
    input_loads.insert(load);
  }
  void memory(llvm::Instruction* i) {
    auto* l = llvm::dyn_cast< llvm::LoadInst >(i);
    auto* s = llvm::dyn_cast< llvm::StoreInst >(i);
    auto* al = dyn_cast_or_null< ar::Load >(original(i));
    auto* as = dyn_cast_or_null< ar::Store >(original(i));
    need((l && al && !l->isVolatile() && !l->isAtomic() && !al->is_volatile() &&
          !al->is_atomic()) ||
             (s && as && !s->isVolatile() && !s->isAtomic() &&
              !as->is_volatile() && !as->is_atomic()),
         "ordinary source/AR memory binding");
    llvm::Value* address = l ? l->getPointerOperand() : s->getPointerOperand();
    auto* type = l ? l->getType() : s->getValueOperand()->getType();
    auto* at = al ? al->result()->type() : as->value()->type();
    ar::Value* ap = al ? al->operand() : as->pointer();
    const auto width =
        module->getDataLayout().getTypeStoreSize(type).getFixedValue();
    need(bundle->data_layout().store_size_in_bytes(at) ==
                 core::ZNumber(width) &&
             (!s || value(as->value(), s->getValueOperand())),
         "memory value/width identity");
    if (auto* global = llvm::dyn_cast< llvm::GlobalVariable >(
            address->stripPointerCasts())) {
      need(value(ap, global), "whole global source/AR address");
      if (global == stderr_global) {
        need(l && type == stderr_global->getValueType() && width == 4 &&
                 at->is_pointer(),
             "stderr pointer rebound or partial access");
        for (auto* user : l->users()) {
          auto* c = llvm::dyn_cast< llvm::CallBase >(user);
          need(c && outputs.count(c) && c->getArgOperand(0) == l,
               "runtime stream pointer escaped/dereferenced");
        }
      } else {
        scalar_global(global);
        need(type == global->getValueType() && at->is_integer() &&
                 cast< ar::IntegerType >(at)->bit_width() ==
                     type->getIntegerBitWidth(),
             "partial global access");
      }
    } else if (auto* base = llvm::dyn_cast< llvm::AllocaInst >(
                   address->stripPointerCasts())) {
      need(value(ap, base) && dom(i->getFunction()).dominates(base, i) &&
               type == base->getAllocatedType() && at->is_integer() &&
               cast< ar::IntegerType >(at)->bit_width() ==
                   type->getIntegerBitWidth(),
           "whole local source/AR access");
      if (pools.count(base)) {
        need(s && type->isIntegerTy(32), "unindexed VLA access");
        seed_range(s, as, base);
      } else {
        auto* count = llvm::dyn_cast< llvm::ConstantInt >(base->getArraySize());
        need(count && count->isOne() && integer(type),
             "fixed scalar local extent");
        storage_uses[base].insert(i);
      }
    } else if (l && type->isPointerTy()) {
      argv_read(l, al);
    } else {
      need(l && type->isIntegerTy(32) && width == 4 && at->is_integer() &&
               cast< ar::IntegerType >(at)->bit_width() == 32,
           "foreign/non-handle indirect memory");
      indexed_range(i, address, ap, al);
      need(!l->user_empty(), "unused handle read");
      for (auto* user : l->users()) {
        auto* c = llvm::dyn_cast< llvm::CallBase >(user);
        need(c && joins.count(c) && c->getArgOperand(0) == l,
             "opaque handle read used outside join");
      }
    }
    consumed.insert(i);
  }
  void function_uses() {
    for (auto* f : reachable) {
      std::vector< llvm::Value* > work{f};
      std::set< llvm::Value* > seen;
      while (!work.empty()) {
        step();
        auto* v = work.back();
        work.pop_back();
        if (!seen.insert(v).second)
          continue;
        for (auto* use : v->users()) {
          if (llvm::isa< llvm::BitCastOperator >(use)) {
            work.push_back(use);
            continue;
          }
          auto* c = llvm::dyn_cast< llvm::CallBase >(use);
          need(c && reachable.count(c->getFunction()) && binding(c),
               "function address escaped frame");
          if (c->getCalledOperand() == v)
            need(f != main && !roots.count(f), "main/root called as helper");
          else
            need(creates.count(c) && roots.count(f) && c->getArgOperand(2) == v,
                 "function pointer used outside actual create");
        }
      }
    }
  }
  void source_frame() {
    for (auto* f : reachable)
      for (auto& b : *f)
        for (auto& i : b) {
          step();
          if (llvm::isa< llvm::DbgInfoIntrinsic >(i))
            continue;
          if (llvm::isa< llvm::LoadInst >(i) || llvm::isa< llvm::StoreInst >(i))
            memory(&i);
          else if (llvm::isa< llvm::CallBase >(i) ||
                   llvm::isa< llvm::AllocaInst >(i))
            need(consumed.count(&i), "unaccounted native effect");
          else if (auto* ret = llvm::dyn_cast< llvm::ReturnInst >(&i)) {
            if (auto* v = ret->getReturnValue())
              need(integer(v->getType()) || (roots.count(f) && null(v)),
                   "pointer/aggregate return escaped frame");
          } else {
            need(llvm::isa< llvm::BinaryOperator >(i) ||
                     llvm::isa< llvm::CastInst >(i) ||
                     llvm::isa< llvm::CmpInst >(i) ||
                     llvm::isa< llvm::PHINode >(i) ||
                     llvm::isa< llvm::SelectInst >(i) ||
                     llvm::isa< llvm::GetElementPtrInst >(i) ||
                     llvm::isa< llvm::BranchInst >(i) ||
                     llvm::isa< llvm::SwitchInst >(i) ||
                     llvm::isa< llvm::FreezeInst >(i) ||
                     llvm::isa< llvm::UnreachableInst >(i),
                 "native operation outside closed effect frame");
          }
        }
    for (const auto& item : originals)
      if (reachable.count(item.first->getFunction()))
        need(consumed.count(item.first), "unaccounted imported effect");
    for (auto* c : creates) {
      auto* a = binding(c);
      indexed_range(c, c->getArgOperand(0), a->argument(0), a);
    }
    for (auto* c : joins) {
      auto* load = llvm::cast< llvm::LoadInst >(c->getArgOperand(0));
      need(consumed.count(load) && load->getFunction() == main &&
               pools.count(pool_base(load->getPointerOperand())),
           "join value has no covered actual pool handle read");
    }
    for (auto* c : scans) {
      need(startup(c), "scan not in serial startup phase");
      auto* input = llvm::dyn_cast< llvm::LoadInst >(c->getArgOperand(0));
      if (input)
        need(input_loads.count(input), "scan input not sealed argv load");
      else
        (void)literal(binding(c)->argument(0), c->getArgOperand(0));
      // This is a real four-byte application write, not a library exemption.
      auto effect = scalar_scan_effect(binding(c));
      auto* g = llvm::dyn_cast< llvm::GlobalVariable >(
          c->getArgOperand(2)->stripPointerCasts());
      need(effect && effect->implicit_state_closed && g && globals.count(g) &&
               effect->call == binding(c) && effect->output == globals.at(g) &&
               peel(binding(c)->argument(ScalarScanEffect::output_argument)) ==
                   globals.at(g) &&
               g->getValueType()->isIntegerTy(32) &&
               ScalarScanEffect::output_bytes == 4,
           "exact whole i32 scan output identity");
      scalar_global(g);
    }
  }
  llvm::AllocaInst* pool_base(llvm::Value* address) const {
    auto* gep =
        llvm::dyn_cast< llvm::GetElementPtrInst >(address->stripPointerCasts());
    return gep ? llvm::dyn_cast< llvm::AllocaInst >(
                     gep->getPointerOperand()->stripPointerCasts())
               : nullptr;
  }
  bool reaches(llvm::BasicBlock* from, llvm::BasicBlock* to) {
    std::vector< llvm::BasicBlock* > work{from};
    std::set< llvm::BasicBlock* > seen;
    while (!work.empty()) {
      step();
      auto* b = work.back();
      work.pop_back();
      if (b == to)
        return true;
      if (!seen.insert(b).second)
        continue;
      work.insert(work.end(), llvm::succ_begin(b), llvm::succ_end(b));
    }
    return false;
  }
  ar::Statement* imported_pure(llvm::Instruction* i) {
    ar::Statement* result = nullptr;
    for (auto* b : *functions.at(i->getFunction())->body())
      for (auto* s : *b) {
        step();
        if (!s->has_frontend() || s->frontend< llvm::Value >() != i)
          continue;
        need(!result, "non-unique imported induction/control operation");
        result = s;
      }
    need(result, "missing imported induction/control operation");
    return result;
  }
  void valid_current_handles() {
    // Internal library-storage obligation only. This pass never writes a
    // numerical result, native handle registry, join digest or completion fact.
    for (const auto& item : pools) {
      auto* storage = item.first;
      std::vector< llvm::CallBase* > producers, consumers;
      for (auto* c : creates)
        if (pool_base(c->getArgOperand(0)) == storage)
          producers.push_back(c);
      for (auto* c : joins) {
        auto* l = llvm::cast< llvm::LoadInst >(c->getArgOperand(0));
        if (pool_base(l->getPointerOperand()) == storage)
          consumers.push_back(c);
      }
      need(producers.size() == 1 && consumers.size() == 1,
           "one create and one consuming join loop per private pool");
      auto* create = producers.front();
      auto* join = consumers.front();
      auto* load = llvm::cast< llvm::LoadInst >(join->getArgOperand(0));
      need(load->hasOneUse(), "handle occurrence has another consumer");
      auto* imported_load = dyn_cast_or_null< ar::Load >(original(load));
      need(imported_load, "actual join load binding");
      indexed_range(join,
                    load->getPointerOperand(),
                    imported_load->operand(),
                    binding(join));
      auto* gep = llvm::cast< llvm::GetElementPtrInst >(
          create->getArgOperand(0)->stripPointerCasts());
      auto* phi = llvm::cast< llvm::PHINode >(gep->getOperand(1));
      auto* loop = loops(main).getLoopFor(phi->getParent());
      auto* latch = loop->getLoopLatch();
      auto* update = llvm::cast< llvm::BinaryOperator >(
          phi->getIncomingValueForBlock(latch));
      auto* header =
          llvm::cast< llvm::BranchInst >(loop->getHeader()->getTerminator());
      auto* cmp = llvm::cast< llvm::ICmpInst >(header->getCondition());
      auto* a_update = imported_pure(update);
      need(dom(main).dominates(create, update) &&
               ar_dominates(binding(create), a_update),
           "iteration can advance without an actual create output");
      // The only normal route to the consumer passes the create loop's
      // count-exhausted edge. Its other exits may not bypass this obligation.
      need(dom(main).dominates(llvm::BasicBlockEdge(loop->getHeader(),
                                                    header->getSuccessor(1)),
                               load->getParent()) &&
               ar_guard(cmp,
                        phi,
                        cmp->getOperand(1),
                        ar::Comparison::SIGE,
                        binding(join)),
           "join can bypass full create-loop coverage");
      for (auto* b : loop->getBlocks())
        for (auto* successor : llvm::successors(b)) {
          if (loop->contains(successor) ||
              (b == loop->getHeader() && successor == header->getSuccessor(1)))
            continue;
          need(!reaches(successor, join->getParent()),
               "early create-loop exit can reach join");
        }
      bool status_guard = false;
      for (auto* user : create->users()) {
        auto* status = llvm::dyn_cast< llvm::ICmpInst >(user);
        if (!status || (status->getPredicate() != llvm::CmpInst::ICMP_EQ &&
                        status->getPredicate() != llvm::CmpInst::ICMP_NE))
          continue;
        llvm::Value* other =
            status->getOperand(0) == create   ? status->getOperand(1)
            : status->getOperand(1) == create ? status->getOperand(0)
                                              : nullptr;
        auto* zero = llvm::dyn_cast_or_null< llvm::ConstantInt >(other);
        if (!zero || !zero->isZero())
          continue;
        for (auto* use : status->users()) {
          auto* branch = llvm::dyn_cast< llvm::BranchInst >(use);
          if (!branch || !branch->isConditional() ||
              branch->getCondition() != status)
            continue;
          const unsigned success =
              status->getPredicate() == llvm::CmpInst::ICMP_EQ ? 0 : 1;
          if (!dom(main).dominates(llvm::BasicBlockEdge(branch->getParent(),
                                                        branch->getSuccessor(
                                                            success)),
                                   update->getParent()) ||
              !(ar_guard(status,
                         status->getOperand(0),
                         status->getOperand(1),
                         ar::Comparison::SIEQ,
                         a_update) ||
                ar_guard(status,
                         status->getOperand(0),
                         status->getOperand(1),
                         ar::Comparison::UIEQ,
                         a_update)))
            continue;
          bool failure_exits = true;
          for (auto* c : joins)
            failure_exits &=
                !reaches(branch->getSuccessor(success ^ 1), c->getParent());
          if (failure_exits)
            status_guard = true;
        }
      }
      need(status_guard, "create failure can advance or reach a join");
      for (auto* use : storage_uses.at(storage))
        if (auto* seed = llvm::dyn_cast< llvm::StoreInst >(use))
          need(!after(create, seed) &&
                   !ar_after(original(create), original(seed)),
               "handle pool overwritten after create begins");
      // Prefix induction: before iteration i, slots [0,i) hold the actual
      // successful creates. The call dominates update, and only rc==0 reaches
      // that update. Every other exit cannot reach any join. At the normal
      // header exit all [0,count) are current joinable handles. Count is
      // stable, all other pool writes are pre-create seeds, and the sole join
      // loop uses 0,+1 with the same count and no wrap. Thus each executed join
      // consumes a distinct current handle once; no other joiner/detach/cancel
      // API exists. This supports the runtime effect envelope even though the
      // AI keeps status TOP and this checker exports no success or
      // happens-before fact.
    }
  }
  void lifetimes() {
    if (pools.empty()) {
      need(saves.empty() && restores.empty(), "unexplained stack lifetime API");
      return;
    }
    need(saves.size() == 1 && restores.size() == 1,
         "one complete stack lifetime frame required");
    auto* save = *saves.begin();
    auto* restore = *restores.begin();
    need(acyclic(save) && acyclic(restore) &&
             restore->getArgOperand(0) == save &&
             dom(main).dominates(save, restore),
         "actual stack save/restore identity");
    for (auto* use : save->users())
      need(use == restore, "stack token escaped");
    for (const auto& item : pools)
      need(dom(main).dominates(save, item.first), "VLA outside saved lifetime");
    for (const auto& item : storage_uses)
      for (auto* use : item.second)
        if (use->getFunction() == main)
          need(!after(restore, use) &&
                   !ar_after(original(restore), original(use)),
               "stack restore can precede a remaining byte effect");
    // No pool base escapes: all its calls are exact create outputs; all loads
    // flow only to join. Workers receive NULL, so no lifetime/HB inference is
    // obtained from the used pthread return status or the join loops.
  }
  void index_import() {
    for (auto it = bundle->global_begin(); it != bundle->global_end(); ++it) {
      step();
      auto* g = *it;
      need(g->has_frontend(), "global without native identity");
      auto* n =
          llvm::dyn_cast< llvm::GlobalVariable >(g->frontend< llvm::Value >());
      need(n && globals.emplace(n, g).second, "global binding not unique");
    }
    for (auto it = bundle->function_begin(); it != bundle->function_end();
         ++it) {
      auto* f = *it;
      if (!f->is_definition())
        continue;
      need(f->has_frontend(), "defined function without native identity");
      auto* source =
          llvm::dyn_cast< llvm::Function >(f->frontend< llvm::Value >());
      need(source && source->arg_size() <= 16 &&
               functions.emplace(source, f).second &&
               source->arg_size() == f->num_parameters(),
           "defined function identity/arity");
      for (auto* b : *f->body())
        for (auto* s : *b) {
          step();
          // The source closure also rejects pointer/integer transport, but a
          // separately imported channel must not acquire a capability from it.
          if (auto* cast = dyn_cast< ar::UnaryOperation >(s))
            need(cast->op() != ar::UnaryOperation::UIToPtr &&
                     cast->op() != ar::UnaryOperation::SIToPtr &&
                     cast->op() != ar::UnaryOperation::PtrToUI &&
                     cast->op() != ar::UnaryOperation::PtrToSI,
                 "imported pointer representation transport");
          if (isa< ar::CallBase >(s) || isa< ar::Load >(s) ||
              isa< ar::Store >(s) || isa< ar::Allocate >(s)) {
            auto* i = s->has_frontend() ? llvm::dyn_cast< llvm::Instruction >(
                                              s->frontend< llvm::Value >())
                                        : nullptr;
            need(i && i->getFunction() == source &&
                     originals.emplace(i, s).second,
                 "memory/call source mapping incomplete or duplicated");
          } else {
            need(isa< ar::Assignment >(s) || isa< ar::UnaryOperation >(s) ||
                     isa< ar::BinaryOperation >(s) ||
                     isa< ar::Comparison >(s) || isa< ar::PointerShift >(s) ||
                     isa< ar::ReturnValue >(s) || isa< ar::Unreachable >(s),
                 "unaccounted imported operation");
          }
        }
    }
  }
  void prove() {
    need(opts.enable_thread_modular &&
             opts.procedural != Procedural::Intraprocedural &&
             opts.entry_points.size() == 1 && opts.no_init_globals.empty() &&
             opts.entry_points.front()->name() == "main" &&
             opts.entry_points.front()->has_frontend() &&
             bundle->data_layout().pointers.bit_width == 32 &&
             !bundle->global_or_null("ar.global_ctors") &&
             !bundle->global_or_null("ar.global_dtors"),
         "supported main/interprocedural ILP32 context");
    main = opts.entry_points.front()->frontend< llvm::Function >();
    module = main->getParent();
    auto* stream = module->getNamedGlobal("stderr");
    need(stream && !stream->use_empty(), "no runtime stderr use");
    const llvm::Triple triple(module->getTargetTriple());
    need(triple.getArch() == llvm::Triple::x86 && triple.isOSLinux() &&
             triple.getEnvironment() == llvm::Triple::GNU &&
             module->getDataLayout().getPointerSizeInBits(0) == 32 &&
             main->getReturnType()->isIntegerTy(32) && main->use_empty() &&
             module->getModuleInlineAsm().empty() && module->alias_empty() &&
             module->ifunc_empty() &&
             !module->getNamedGlobal("llvm.global_ctors") &&
             !module->getNamedGlobal("llvm.global_dtors") &&
             mutex_effect_pointer_representations_closed(*module),
         "closed GNU source module");
    need(main->arg_size() == 0 || main->arg_size() == 2,
         "main input signature");
    if (main->arg_size() == 2) {
      auto* p = llvm::dyn_cast< llvm::PointerType >(main->getArg(1)->getType());
      auto* q =
          p && !p->isOpaque()
              ? llvm::dyn_cast< llvm::PointerType >(p->getPointerElementType())
              : nullptr;
      need(main->getArg(0)->getType()->isIntegerTy(32) && p &&
               p->getAddressSpace() == 0 && q && !q->isOpaque() &&
               q->getAddressSpace() == 0 &&
               q->getPointerElementType()->isIntegerTy(8),
           "actual argc/argv ABI");
    }
    index_import();
    stderr_storage();
    discover(main);
    need(!creates.empty() && creates.size() <= 8 && !outputs.empty(),
         "finite create-site/stream inventory");
    function_uses();
    allocations();
    source_frame();
    valid_current_handles();
    lifetimes();
    for (auto& g : module->globals()) {
      if (g.use_empty())
        continue;
      if (&g == stderr_global)
        continue;
      if (g.isConstant()) {
        need(globals.count(&g), "constant global mapping");
        (void)literal(globals.at(&g), &g);
      } else
        scalar_global(&g);
    }
    // Seal both capabilities only after every application and runtime effect,
    // pointer representation use, range and lifetime obligation has succeeded.
    // Whole integer type alone never supplies the non-exposure capability.
    for (auto* c : scans) {
      auto* g = llvm::cast< llvm::GlobalVariable >(
          c->getArgOperand(2)->stripPointerCasts());
      scan_certificates.emplace(binding(c), globals.at(g));
    }
    for (auto* c : outputs)
      output_certificates.insert(binding(c));
    valid = true;
  }

public:
  Impl(const AnalysisOptions& options, ar::Bundle* input)
      : opts(options), bundle(input) {
    // Construction is pure with respect to Context. Even an already polluted
    // MAY escape board cannot affect this source proof; no flag is cleared.
    try {
      prove();
    } catch (const Refuse& r) {
      refusal = r.reason;
    } catch (const std::out_of_range&) {
      refusal = "missing source/AR identity";
    }
    if (!valid) {
      output_certificates.clear();
      scan_certificates.clear();
    }
  }
  bool omits(ar::CallBase* call, CallContext* context) const {
    if (!valid || !context || !output_certificates.count(call))
      return false;
    for (auto* cc = context; !cc->empty(); cc = cc->parent()) {
      auto* caller = dyn_cast< ar::CallBase >(cc->call());
      if (!caller || !admitted_calls.count(caller))
        return false;
    }
    return true;
  }
  ar::GlobalVariable* non_exposing(ar::CallBase* call) const {
    auto found = scan_certificates.find(call);
    return valid && found != scan_certificates.end() ? found->second : nullptr;
  }
  JsonDict info() const {
    return {{"runtime_stdio_contract",
             "closed-bounded-application-frame-stderr-v1"},
            {"scope",
             "runtime stderr write and exact scalar-scan non-exposure; no "
             "pthread success, handle value, join or HB fact"},
            {"scalar_scan_non_exposure_contract",
             "whole-program-scalar-representation-closure-v1"},
            {"stack_range_contract", "native-ar-counted-i32-stack-effects-v1"},
            {"allocation_contract",
             "existing interprocedural Allocate unsigned MulNoWrap normal "
             "continuation"},
            {"pthread_scope",
             "private all-cell successful-create coverage; distinct current "
             "handle consumption; no exported HB"}};
  }
  const std::string& why() const { return refusal; }
};
RuntimeStdioFrame::RuntimeStdioFrame(Context& ctx)
    : _impl(new Impl(ctx.opts, ctx.bundle)) {}
RuntimeStdioFrame::~RuntimeStdioFrame() = default;
bool RuntimeStdioFrame::omits_stderr_write(ar::CallBase* c,
                                           CallContext* cc) const {
  return _impl->omits(c, cc);
}
ar::GlobalVariable* RuntimeStdioFrame::non_exposing_scan_output(
    ar::CallBase* c) const {
  return _impl->non_exposing(c);
}
JsonDict RuntimeStdioFrame::info(ar::CallBase*) const {
  return _impl->info();
}
const std::string& RuntimeStdioFrame::refusal_reason() const {
  return _impl->why();
}
void configure_runtime_stdio_frame(Context& ctx) {
  if (!ctx.runtime_stdio_frame_certificate()) {
    ctx._runtime_stdio_frame_certificate =
        std::make_shared< const RuntimeStdioFrame >(ctx);
    const auto& reason = ctx.runtime_stdio_frame_certificate()->refusal_reason();
    if (!reason.empty())
      log::debug("Runtime stdio frame refused: " + reason);
  }
}
} // namespace analyzer
} // namespace ikos
