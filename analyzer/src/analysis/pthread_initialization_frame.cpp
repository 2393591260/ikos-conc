// Copyright and license: see context.hpp.
#include <ikos/analyzer/analysis/pthread_initialization_frame.hpp>
#include <ikos/analyzer/analysis/source_call_binding.hpp>
#include <ikos/analyzer/util/log.hpp>
#include <llvm/Analysis/ConstantFolding.h>
#include <llvm/IR/Operator.h>
#include <algorithm>
#include <map>
#include <set>

namespace ikos {
namespace analyzer {
namespace {
using core::ZNumber;
struct Refuse {};
using Address = std::pair< unsigned, ZNumber >;
struct Root {
  llvm::Value* source;
  ar::CallBase* allocation;
  ar::GlobalVariable* global;
  ZNumber bytes;
};
struct Access { Address address; ZNumber bytes; bool relay_ok = false; };
struct Frame {
  std::map< llvm::Value*, Address > pointers;
  std::set< Address > held;
};
ZNumber integer(const llvm::APInt& value, bool sign) {
  llvm::SmallString< 64 > text;
  value.toString(text, 10, sign);
  return ZNumber::from_string(text.str().str());
}
}
class PthreadInitializationFrame::Impl {
  Context& ctx;
  llvm::Module* module = nullptr;
  llvm::Function* main = nullptr;
  std::map< llvm::Value*, ar::Statement* > originals;
  std::map< llvm::Function*, ar::Function* > functions;
  std::map< llvm::GlobalVariable*, ar::GlobalVariable* > globals;
  std::map< llvm::GlobalVariable*, Address > relays;
  std::map< llvm::Value*, unsigned > identities;
  std::vector< Root > roots{{nullptr, nullptr, nullptr, ZNumber(0)}};
  std::vector< Access > ordinary;
  std::set< ar::Statement* > consumed;
  std::map< Address, unsigned > attributes, mutexes;
  std::set< unsigned > born;
  std::set< llvm::Function* > active, visited;
  std::vector< llvm::Function* > workers;
  std::map< ar::CallBase*, PthreadInitializationEffect > effects;
  unsigned budget = 8192;
  bool spawned = false, valid = false, heap_mutex = false;
  std::string refusal;

  void need(bool condition, const char* reason) {
    if (!condition) { refusal = reason; throw Refuse{}; }
  }
  void step() { need(budget != 0, "constant byte-frame budget"); --budget; }
  ar::Statement* original(llvm::Value* value) const {
    auto i = originals.find(value);
    return i == originals.end() ? nullptr : i->second;
  }
  bool value(ar::Value* a, llvm::Value* source) const {
    for (unsigned depth = 0; depth < 3 && a; ++depth) {
      if (auto* constant = llvm::dyn_cast< llvm::ConstantInt >(source)) {
        auto* actual = dyn_cast< ar::IntegerConstant >(a);
        return actual && actual->value().bit_width() == constant->getBitWidth() &&
            actual->value().cast(actual->value().bit_width(), core::Unsigned).to_z_number() == integer(constant->getValue(), false);
      }
      if (llvm::isa< llvm::ConstantPointerNull >(source)) return isa< ar::NullConstant >(a);
      auto* stripped = source->getType()->isPointerTy() ? source->stripPointerCasts() : source;
      if (auto* function = llvm::dyn_cast< llvm::Function >(stripped)) {
        if (auto* pointer = dyn_cast< ar::FunctionPointerConstant >(a))
          return pointer->function()->has_frontend() && pointer->function()->frontend< llvm::Value >() == function;
      }
      if (auto* variable = dyn_cast< ar::Variable >(a))
        if (variable->has_frontend() &&
            (variable->frontend< llvm::Value >() == source || variable->frontend< llvm::Value >() == stripped)) return true;
      auto* statement = original(source);
      if (statement && statement->result_or_null() == a) return true;
      if (auto* argument = llvm::dyn_cast< llvm::Argument >(source)) {
        auto function = functions.find(argument->getParent());
        if (function != functions.end() && function->second->param(argument->getArgNo()) == a) return true;
      }
      auto* variable = dyn_cast< ar::InternalVariable >(a);
      auto* definition = variable ? unique_def(variable) : nullptr;
      if (auto* copy = dyn_cast_or_null< ar::Assignment >(definition)) a = copy->operand();
      else if (auto* cast = dyn_cast_or_null< ar::UnaryOperation >(definition)) {
        if (cast->op() != ar::UnaryOperation::Bitcast) return false;
        a = cast->operand();
      } else return false;
    }
    return false;
  }
  bool width(ar::Type* a, llvm::Type* source) const {
    return ctx.bundle->data_layout().store_size_in_bytes(a) ==
        module->getDataLayout().getTypeStoreSize(source).getFixedValue();
  }
  unsigned global(llvm::GlobalVariable* g) {
    auto i = identities.find(g);
    if (i != identities.end()) return i->second;
    need(globals.count(g) && g->hasInitializer() && !g->isThreadLocal() &&
         !g->isExternallyInitialized() && g->getAddressSpace() == 0 &&
         g->getInitializer()->isNullValue(), "global storage origin");
    auto bytes = module->getDataLayout().getTypeAllocSize(g->getValueType()).getFixedValue();
    need(ctx.bundle->data_layout().alloc_size_in_bytes(globals.at(g)->type()->pointee()) == bytes,
         "global source/AR layout");
    unsigned id = roots.size();
    roots.push_back({g, nullptr, globals.at(g), ZNumber(bytes)});
    identities.emplace(g, id);
    return id;
  }
  // Only layout expressions with literal/NULL leaves; actual pointers never fold.
  llvm::Constant* layout(llvm::Value* value, unsigned depth = 0) {
    step(); need(depth < 24, "layout expression depth");
    if (auto* c = llvm::dyn_cast< llvm::ConstantInt >(value)) return c;
    if (auto* c = llvm::dyn_cast< llvm::ConstantPointerNull >(value)) return c;
    auto* i = llvm::dyn_cast< llvm::Instruction >(value);
    auto* e = llvm::dyn_cast< llvm::ConstantExpr >(value);
    need(i || e, "nonconstant layout input");
    unsigned op = i ? i->getOpcode() : e->getOpcode();
    need(op == llvm::Instruction::GetElementPtr || op == llvm::Instruction::PtrToInt ||
         op == llvm::Instruction::BitCast || op == llvm::Instruction::Sub,
         "unsupported layout expression");
    std::vector< llvm::Constant* > operands;
    auto* user = llvm::cast< llvm::User >(value);
    for (auto& operand : user->operands()) operands.push_back(layout(operand.get(), depth + 1));
    llvm::Constant* result = i ? llvm::ConstantFoldInstOperands(i, operands, module->getDataLayout())
        : llvm::ConstantFoldConstant(e->getWithOperands(operands), module->getDataLayout());
    need(result && !llvm::isa< llvm::UndefValue >(result) &&
         !llvm::isa< llvm::PoisonValue >(result), "indeterminate layout constant");
    return result;
  }
  Address pointer(llvm::Value* value, Frame& frame) {
    if (llvm::isa< llvm::ConstantPointerNull >(value)) return {0, ZNumber(0)};
    auto known = frame.pointers.find(value);
    if (known != frame.pointers.end()) return known->second;
    if (auto* g = llvm::dyn_cast< llvm::GlobalVariable >(value)) return {global(g), ZNumber(0)};
    if (auto* cast = llvm::dyn_cast< llvm::BitCastOperator >(value))
      return pointer(cast->getOperand(0), frame);
    auto* gep = llvm::dyn_cast< llvm::GEPOperator >(value);
    need(gep && gep->getPointerAddressSpace() == 0, "unknown pointer source");
    auto base = pointer(gep->getPointerOperand(), frame);
    // NULL-based layout expressions are never entered into the address map.
    need(base.first != 0, "layout token is not a memory address");
    llvm::APInt delta(32, 0);
    bool exact = gep->accumulateConstantOffset(module->getDataLayout(), delta,
        [&](llvm::Value& index, llvm::APInt& out) {
          auto* c = llvm::dyn_cast< llvm::ConstantInt >(layout(&index));
          if (!c) return false;
          out = c->getValue().sextOrTrunc(32); return true;
        });
    need(exact, "nonconstant byte displacement");
    base.second += integer(delta, true);
    need(base.second >= 0 && base.second <= roots.at(base.first).bytes, "pointer byte bounds");
    return base;
  }
  void range(const Address& a, const ZNumber& bytes) {
    need(a.first && bytes > 0 && a.second >= 0 &&
         a.second + bytes <= roots.at(a.first).bytes, "memory byte bounds");
  }
  bool typed(llvm::Value* value, const char* name, unsigned bytes) {
    auto* p = llvm::dyn_cast< llvm::PointerType >(value->getType());
    auto* t = p ? llvm::dyn_cast< llvm::StructType >(p->getPointerElementType()) : nullptr;
    return p && p->getAddressSpace() == 0 && t && !t->isOpaque() && t->hasName() &&
        t->getName() == name && module->getDataLayout().getTypeAllocSize(t) == bytes;
  }
  void aligned(const Address& a) {
    need(a.second % 4 == 0, "runtime leaf alignment");
    if (auto* g = llvm::dyn_cast< llvm::GlobalVariable >(roots.at(a.first).source)) {
      auto alignment = g->getAlign() ? g->getAlign()->value()
          : module->getDataLayout().getABITypeAlign(g->getValueType()).value();
      need(alignment >= 4, "global runtime alignment");
    } else need(roots.at(a.first).allocation != nullptr, "runtime storage lifetime");
  }
  ar::CallBase* binding(llvm::CallBase* c) {
    auto* a = source_call_binding(c, original(c));
    need(a != nullptr, "source/AR call binding");
    for (unsigned n = 0; n < c->arg_size(); ++n)
      need(value(a->argument(n), c->getArgOperand(n)) && width(a->argument(n)->type(), c->getArgOperand(n)->getType()),
           "source/AR actual argument");
    need(c->getType()->isVoidTy() ? !a->has_result() :
         a->has_result() && value(a->result(), c) && width(a->result()->type(), c->getType()),
         "source/AR call result");
    consumed.insert(a);
    return a;
  }
  void effect(ar::CallBase* call, const Address& dest, unsigned bytes, unsigned read) {
    const auto& root = roots.at(dest.first);
    auto inserted = effects.emplace(call, PthreadInitializationEffect{bytes, read, {}});
    auto& description = inserted.first->second;
    need(description.write_bytes == bytes && description.read_bytes == read, "effect shape mismatch");
    description.destinations.push_back({root.allocation, root.global, dest.second});
  }
  void call(llvm::CallBase* c, Frame& frame, bool startup) {
    auto* a = binding(c);
    auto* f = llvm::dyn_cast< llvm::Function >(c->getCalledOperand()->stripPointerCasts());
    auto* target = native_handle_callee(a);
    const auto name = f->getName();
    if (!f->isDeclaration()) {
      need(functions.count(f) && target == functions.at(f), "helper identity");
      Frame child;
      for (unsigned n = 0; n < c->arg_size(); ++n)
        if (f->getArg(n)->getType()->isPointerTy())
          child.pointers.emplace(f->getArg(n), pointer(c->getArgOperand(n), frame));
      child.held = frame.held;
      walk(f, child, startup);
      frame.held = std::move(child.held);
      return;
    }
    if (c->arg_empty() && c->getType()->isIntegerTy() &&
        target->intrinsic_id() == ar::Intrinsic::IkosNonDet &&
        c->getMetadata("ikos.freeze_uninit") &&
        (name == "__ikos_nondet_int" || name == "__ikos_nondet_int64")) return;
    if (name == "malloc") {
      auto* size = c->arg_size() == 1 ? llvm::dyn_cast< llvm::ConstantInt >(c->getArgOperand(0)) : nullptr;
      need(startup && !spawned && c->getFunction() == main && size && size->getBitWidth() == 32 &&
           size->getZExtValue() > 0 && size->getZExtValue() <= 1048576 &&
           target->intrinsic_id() == ar::Intrinsic::LibcMalloc && c->getType()->isPointerTy(),
           "one-shot constant allocation");
      unsigned id = roots.size();
      roots.push_back({c, a, nullptr, integer(size->getValue(), false)});
      frame.pointers.emplace(c, Address{id, ZNumber(0)});
      return;
    }
    bool attr_init = name == "pthread_mutexattr_init";
    bool attr_set = name == "pthread_mutexattr_settype";
    bool mutex_init = name == "pthread_mutex_init";
    if (attr_init || attr_set || mutex_init) {
      need(startup && !spawned && c->getType()->isIntegerTy(32) &&
           target->has_frontend() && target->frontend< llvm::Value >() == f &&
           target->name() == name.str() && c->arg_size() == (attr_init ? 1u : 2u),
           "initialization declaration/prefix");
      auto dest = pointer(c->getArgOperand(0), frame);
      range(dest, ZNumber(mutex_init ? 24 : 4)); aligned(dest);
      need(typed(c->getArgOperand(0), mutex_init ? "union.pthread_mutex_t" : "union.pthread_mutexattr_t",
                 mutex_init ? 24 : 4), "runtime opaque type/layout");
      if (attr_init) {
        need(roots.at(dest.first).global && dest.second == 0 &&
             roots.at(dest.first).bytes == 4 && !attributes.count(dest), "attribute lifetime/one init");
        attributes.emplace(dest, 0);
      } else if (attr_set) {
        auto* kind = llvm::dyn_cast< llvm::ConstantInt >(c->getArgOperand(1));
        need(attributes.count(dest) && kind && kind->getBitWidth() == 32 &&
             (kind->isZero() || kind->equalsInt(2)), "default attribute flags/type");
        attributes.at(dest) = kind->getZExtValue();
      } else {
        unsigned kind = 0;
        bool nonnull = !llvm::isa< llvm::ConstantPointerNull >(c->getArgOperand(1));
        if (nonnull) {
          auto attr = pointer(c->getArgOperand(1), frame);
          need(attributes.count(attr) && typed(c->getArgOperand(1), "union.pthread_mutexattr_t", 4),
               "initialized attribute input");
          kind = attributes.at(attr);
        }
        need(roots.at(dest.first).allocation && born.count(dest.first) &&
             mutexes.emplace(dest, kind).second, "live heap birth and distinct mutex initialization");
        heap_mutex = true;
      }
      effect(a, dest, mutex_init ? 24 : 4,
             mutex_init && !llvm::isa< llvm::ConstantPointerNull >(c->getArgOperand(1)) ? 4 : 0);
      return;
    }
    if (name == "pthread_mutex_lock" || name == "pthread_mutex_unlock") {
      need(c->arg_size() == 1 && c->getType()->isIntegerTy(32) &&
           target->intrinsic_id() == (name == "pthread_mutex_lock" ?
             ar::Intrinsic::PthreadMutexLock : ar::Intrinsic::PthreadMutexUnlock), "mutex API identity");
      auto dest = pointer(c->getArgOperand(0), frame);
      need(mutexes.count(dest) && typed(c->getArgOperand(0), "union.pthread_mutex_t", 24),
           "initialized mutex consumer");
      if (name == "pthread_mutex_lock")
        need(frame.held.insert(dest).second, "repeated acquisition");
      else if (!frame.held.erase(dest))
        need(mutexes.at(dest) == 2, "wrong NORMAL unlock");
      return;
    }
    if (name == "pthread_create") {
      need(startup && c->getFunction() == main, "create activation");
      need(native_handle_api(c, true), "native create ABI");
      need(target->intrinsic_id() == ar::Intrinsic::PthreadCreate ||
           (target->has_frontend() && target->frontend< llvm::Value >() == f &&
            target->name() == "pthread_create"), "AR create identity");
      need(unread_private_thread_output(a), "private create output/callback");
      auto output = pointer(c->getArgOperand(0), frame); range(output, ZNumber(4));
      auto* storage = llvm::dyn_cast< llvm::AllocaInst >(roots.at(output.first).source);
      need(storage && output.second == 0 && roots.at(output.first).bytes == 4 &&
           storage->getAlign().value() >= 4, "private native output bounds");
      auto* worker = llvm::cast< llvm::Function >(c->getArgOperand(2)->stripPointerCasts());
      workers.push_back(worker); spawned = true; return;
    }
    need(false, "uncovered library effect");
  }
  void walk(llvm::Function* f, Frame& frame, bool startup) {
    need(functions.count(f) && f->size() == 1 && active.size() < 16 && active.insert(f).second &&
         f->getCallingConv() == llvm::CallingConv::C && !f->isVarArg(), "finite direct helper tree");
    visited.insert(f);
    for (auto& i : f->getEntryBlock()) {
      step();
      if (auto* c = llvm::dyn_cast< llvm::CallBase >(&i)) call(c, frame, startup);
      else if (auto* alloca = llvm::dyn_cast< llvm::AllocaInst >(&i)) {
        auto* count = llvm::dyn_cast< llvm::ConstantInt >(alloca->getArraySize());
        need(f == main && alloca->getAllocatedType()->isIntegerTy(32) && count && count->isOne() &&
             alloca->getAddressSpace() == 0, "private scalar stack storage");
        unsigned id = roots.size(); roots.push_back({alloca, nullptr, nullptr, ZNumber(4)});
        frame.pointers.emplace(alloca, Address{id, ZNumber(0)});
      } else if (auto* store = llvm::dyn_cast< llvm::StoreInst >(&i)) {
        auto* actual = dyn_cast_or_null< ar::Store >(original(store));
        need(actual && !store->isAtomic() && !store->isVolatile() &&
             !actual->is_atomic() && !actual->is_volatile() &&
             value(actual->pointer(), store->getPointerOperand()) &&
             value(actual->value(), store->getValueOperand()) && width(actual->value()->type(), store->getValueOperand()->getType()),
             "store correspondence");
        consumed.insert(actual);
        auto dest = pointer(store->getPointerOperand(), frame);
        ZNumber bytes(module->getDataLayout().getTypeStoreSize(store->getValueOperand()->getType()).getFixedValue());
        range(dest, bytes); ordinary.push_back({dest, bytes});
        if (store->getValueOperand()->getType()->isPointerTy()) {
          auto* g = llvm::dyn_cast< llvm::GlobalVariable >(roots.at(dest.first).source);
          need(startup && !spawned && g && g->getValueType()->isPointerTy() &&
               dest.second == 0 && bytes == 4 && !relays.count(g), "immutable global pointer relay");
          auto origin = pointer(store->getValueOperand(), frame);
          need(origin.first && roots.at(origin.first).allocation && origin.second == 0,
               "relay allocation origin");
          relays.emplace(g, origin);
          ordinary.back().relay_ok = true;
        } else {
          need(store->getValueOperand()->getType()->isIntegerTy(), "noninteger application effect");
          if (startup && !spawned && roots.at(dest.first).allocation && dest.second == 0)
            born.insert(dest.first);
        }
      } else if (auto* load = llvm::dyn_cast< llvm::LoadInst >(&i)) {
        auto* actual = dyn_cast_or_null< ar::Load >(original(load));
        need(actual && !load->isAtomic() && !load->isVolatile() &&
             !actual->is_atomic() && !actual->is_volatile() && value(actual->operand(), load->getPointerOperand()) &&
             value(actual->result(), load) && width(actual->result()->type(), load->getType()), "load correspondence");
        consumed.insert(actual);
        auto source = pointer(load->getPointerOperand(), frame);
        ZNumber bytes(module->getDataLayout().getTypeStoreSize(load->getType()).getFixedValue());
        range(source, bytes); ordinary.push_back({source, bytes});
        if (load->getType()->isPointerTy()) {
          auto* g = llvm::dyn_cast< llvm::GlobalVariable >(roots.at(source.first).source);
          need(g && relays.count(g) && source.second == 0 && bytes == 4, "closed pointer load");
          frame.pointers.emplace(load, relays.at(g));
          ordinary.back().relay_ok = true;
        } else need(load->getType()->isIntegerTy(), "noninteger application load");
      } else if (auto* ret = llvm::dyn_cast< llvm::ReturnInst >(&i)) {
        auto* value = ret->getReturnValue();
        need(!value || value->getType()->isIntegerTy() || llvm::isa< llvm::ConstantPointerNull >(value),
             "pointer return escape");
      } else if (i.getType()->isPointerTy()) {
        // An unused NULL-based layout token cannot serve as a memory address.
        auto* gep = llvm::dyn_cast< llvm::GEPOperator >(&i);
        if (gep && llvm::isa< llvm::ConstantPointerNull >(gep->getPointerOperand())) layout(&i);
        else frame.pointers.emplace(&i, pointer(&i, frame));
      } else if (llvm::isa< llvm::PtrToIntInst >(&i)) layout(&i);
      else {
        need(llvm::isa< llvm::BinaryOperator >(&i) || llvm::isa< llvm::CastInst >(&i), "unsupported CFG/instruction");
        for (auto& operand : i.operands()) need(!operand->getType()->isPointerTy(), "numeric pointer escape");
      }
    }
    active.erase(f);
  }
  void run() {
    need(ctx.opts.enable_thread_modular && ctx.opts.use_checks &&
         std::find(ctx.opts.analyses.begin(), ctx.opts.analyses.end(), CheckerName::DataRace) != ctx.opts.analyses.end() &&
         ctx.opts.entry_points.size() == 1 &&
         ctx.opts.entry_points.front()->name() == "main" && ctx.opts.entry_points.front()->num_parameters() == 0 &&
         ctx.opts.no_init_globals.empty() && ctx.bundle->data_layout().pointers.bit_width == 32 &&
         !ctx.bundle->global_or_null("ar.global_ctors") && !ctx.bundle->global_or_null("ar.global_dtors"),
         "entry/ABI/options");
    for (auto it = ctx.bundle->function_begin(); it != ctx.bundle->function_end(); ++it) {
      auto* f = *it;
      if (!f->is_definition()) continue;
      need(f->has_frontend(), "function source");
      auto* source = f->frontend< llvm::Function >(); functions.emplace(source, f);
      if (f == ctx.opts.entry_points.front()) { main = source; module = source->getParent(); }
      for (auto* block : *f->body()) for (auto* s : *block) {
        step();
        if (isa< ar::Load >(s) || isa< ar::Store >(s) || isa< ar::CallBase >(s))
          need(s->has_frontend() && originals.emplace(s->frontend< llvm::Value >(), s).second,
               "complete source memory/call mapping");
      }
    }
    need(main && main->use_empty() && module->getModuleInlineAsm().empty(), "closed main/representations");
    llvm::Triple triple(module->getTargetTriple());
    need(triple.getArch() == llvm::Triple::x86 && triple.isOSLinux() &&
         triple.getEnvironment() == llvm::Triple::GNU && module->getDataLayout().getPointerSizeInBits() == 32,
         "GNU i386 runtime");
    for (auto it = ctx.bundle->global_begin(); it != ctx.bundle->global_end(); ++it) {
      auto* g = *it;
      if (g->has_frontend()) globals.emplace(g->frontend< llvm::GlobalVariable >(), g);
    }
    Frame frame; walk(main, frame, true);
    need(heap_mutex && !workers.empty(), "no heap initialization use");
    for (auto* worker : workers) {
      Frame child; child.pointers.emplace(worker->getArg(0), Address{0, ZNumber(0)});
      walk(worker, child, false);
    }
    for (auto* function : visited)
      for (auto* block : *functions.at(function)->body()) for (auto* statement : *block)
        if (isa< ar::Load >(statement) || isa< ar::Store >(statement) || isa< ar::CallBase >(statement))
          need(consumed.count(statement), "unconsumed AR memory/call effect");
    for (const auto& access : ordinary) {
      for (const auto& relay : relays) {
        auto id = identities.at(relay.first);
        need(access.address.first != id || access.address.second >= 4 || access.relay_ok,
             "ordinary pointer-relay representation access");
      }
      for (const auto& leaf : mutexes)
        need(access.address.first != leaf.first.first || access.address.second + access.bytes <= leaf.first.second ||
             leaf.first.second + 24 <= access.address.second, "ordinary runtime mutex-byte access");
      for (const auto& leaf : attributes)
        need(access.address.first != leaf.first.first || access.address.second + access.bytes <= leaf.first.second ||
             leaf.first.second + 4 <= access.address.second, "ordinary attribute-byte access");
    }
    for (auto a = mutexes.begin(); a != mutexes.end(); ++a)
      for (auto b = std::next(a); b != mutexes.end(); ++b)
        need(a->first.first != b->first.first || a->first.second + 24 <= b->first.second ||
             b->first.second + 24 <= a->first.second, "overlapping runtime mutex leaves");
    valid = true;
  }
public:
  explicit Impl(Context& context) : ctx(context) {
    try { run(); } catch (const Refuse&) { effects.clear(); }
  }
  const PthreadInitializationEffect* get(ar::CallBase* call) const {
    auto i = effects.find(call);
    return valid && i != effects.end() ? &i->second : nullptr;
  }
  const std::string& reason() const { return refusal; }
};
bool PthreadInitializationEffect::matches(MemoryLocation* location, const ZNumber& offset) const {
  for (const auto& target : destinations) {
    if (offset != target.offset) continue;
    if (auto* heap = dyn_cast< DynAllocMemoryLocation >(location)) {
      if (target.allocation && heap->call() == target.allocation && heap->context()->empty()) return true;
    } else if (auto* global = dyn_cast< GlobalMemoryLocation >(location))
      if (target.global && global->global_var() == target.global) return true;
  }
  return false;
}
PthreadInitializationFrame::PthreadInitializationFrame(Context& ctx) : _impl(new Impl(ctx)) {}
PthreadInitializationFrame::~PthreadInitializationFrame() = default;
const PthreadInitializationEffect* PthreadInitializationFrame::effect(ar::CallBase* c) const { return _impl->get(c); }
const std::string& PthreadInitializationFrame::refusal_reason() const { return _impl->reason(); }
void configure_pthread_initialization_frame(Context& ctx) {
  if (ctx.pthread_initialization_frame) return;
  ctx.pthread_initialization_frame = std::make_shared< const PthreadInitializationFrame >(ctx);
  if (!ctx.pthread_initialization_frame->refusal_reason().empty())
    log::debug("Heap pthread initialization frame refused: " + ctx.pthread_initialization_frame->refusal_reason());
}
} // namespace analyzer
} // namespace ikos
