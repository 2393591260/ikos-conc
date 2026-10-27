#pragma once
// Included inside bmc after dynamic reaching_definition(). No new execution engine.
struct NativeSCGroup {
  std::size_t begin = 0, end = 0;
  NativeSCSource source;
};
inline bool native_sc_integer(ar::Value* value, unsigned bits) {
  auto* t = core::dyn_cast< ar::IntegerType >(value->type());
  return t && t->bit_width() == bits;
}
inline bool native_sc_groups(const std::vector< BmcEvent >& path,
                             std::vector< NativeSCGroup >& groups,
                             std::vector< std::size_t >& heads) {
  heads.resize(path.size());
  for (std::size_t i = 0; i < path.size(); ++i) heads[i] = i;
  for (std::size_t i = 0; i < path.size();) {
    auto* s = path[i].stmt;
    auto* raw = native_sc_source(s);
    const bool atomic = llvm::isa_and_nonnull< llvm::AtomicRMWInst >(raw) ||
        llvm::isa_and_nonnull< llvm::AtomicCmpXchgInst >(raw) ||
        (llvm::isa_and_nonnull< llvm::LoadInst >(raw) && llvm::cast< llvm::LoadInst >(raw)->isAtomic()) ||
        (llvm::isa_and_nonnull< llvm::StoreInst >(raw) && llvm::cast< llvm::StoreInst >(raw)->isAtomic());
    if (!atomic) {++i; continue;}
    auto d = native_sc_source_descriptor(s);
    if (!d || std::getenv("IKOS_BMC_DISABLE_NATIVE_SC")) return false;
    std::size_t cursor = i;
    auto member = [&]() -> ar::Statement* {
      if (cursor == path.size() || path[cursor].synthetic_atomic || path[cursor].synthetic_condition ||
          path[cursor].once_stage != OnceStage::None || path[cursor].candidate ||
          native_sc_source(path[cursor].stmt) != d.instruction) return nullptr;
      return path[cursor++].stmt;
    };
    auto load_ok = [&](ar::Load* l) {
      return l && l->ordering() == ar::AtomicOrdering::SeqCst &&
          l->is_volatile() == d.volatile_access && l->alignment() == d.alignment &&
          native_sc_integer(l->result(), d.bits) && native_sc_operand(l->operand(), d.pointer);
    };
    auto store_ok = [&](ar::Store* w, ar::Value* pointer) {
      return w && w->ordering() == ar::AtomicOrdering::SeqCst &&
          w->is_volatile() == d.volatile_access && w->alignment() == d.alignment &&
          native_sc_integer(w->value(), d.bits) && w->pointer() == pointer;
    };
    if (std::string(d.operation) == "load") {
      if (!load_ok(core::dyn_cast_or_null< ar::Load >(member()))) return false;
    } else if (std::string(d.operation) == "store") {
      auto* w = core::dyn_cast_or_null< ar::Store >(member());
      if (!w || !store_ok(w, w->pointer()) || !native_sc_operand(w->pointer(), d.pointer) ||
          !native_sc_operand(w->value(), d.argument)) return false;
    } else {
      auto* l = core::dyn_cast_or_null< ar::Load >(member());
      if (!load_ok(l)) return false;
      if (std::string(d.operation) == "cas") {
        auto* cx = llvm::cast< llvm::AtomicCmpXchgInst >(d.instruction);
        ar::Comparison* guard = nullptr;
        if (cursor < path.size() && core::isa< ar::Comparison >(path[cursor].stmt))
          guard = core::dyn_cast_or_null< ar::Comparison >(member());
        auto* flag = core::dyn_cast_or_null< ar::Assignment >(member());
        auto* bit = flag ? core::dyn_cast< ar::IntegerConstant >(flag->operand()) : nullptr;
        if (!flag || !bit || !native_sc_integer(bit, 1) || !native_sc_integer(flag->result(), 1)) return false;
        const bool success = bit->value().to_z_number() != 0;
        if (success != 0 || !d.weak) {
          if (!guard || guard->left() != l->result() || !native_sc_operand(guard->right(), d.expected) ||
              guard->predicate() != (success != 0 ? ar::Comparison::SIEQ : ar::Comparison::SINE)) return false;
        } else if (guard) return false;
        if (success != 0) {
          auto* w = core::dyn_cast_or_null< ar::Store >(member());
          if (!store_ok(w, l->operand()) || !native_sc_operand(w->value(), d.argument)) return false;
        }
        auto* a = core::dyn_cast_or_null< ar::InsertElement >(member());
        auto* b = core::dyn_cast_or_null< ar::InsertElement >(member());
        auto* ao = a ? core::dyn_cast< ar::IntegerConstant >(a->offset()) : nullptr;
        auto* bo = b ? core::dyn_cast< ar::IntegerConstant >(b->offset()) : nullptr;
        const auto* layout = cx->getModule()->getDataLayout().getStructLayout(
            llvm::cast< llvm::StructType >(cx->getType()));
        if (!a || !b || !ao || !bo || !core::isa< ar::UndefinedConstant >(a->aggregate()) ||
            a->element() != l->result() || b->aggregate() != a->result() || b->element() != flag->result() ||
            ao->value().to_z_number() != layout->getElementOffset(0) ||
            bo->value().to_z_number() != layout->getElementOffset(1)) return false;
      } else {
        ar::Value* stored = nullptr;
        if (std::string(d.operation) != "xchg") {
          auto* b = core::dyn_cast_or_null< ar::BinaryOperation >(member());
          if (!b || b->left() != l->result() || !native_sc_operand(b->right(), d.argument) ||
              !native_sc_integer(b->result(), d.bits) || b->has_no_wrap() || b->is_exact()) return false;
          const auto op = b->op(); const std::string name(d.operation);
          if (!((name == "add" && (op == ar::BinaryOperation::SAdd || op == ar::BinaryOperation::UAdd)) ||
                (name == "sub" && (op == ar::BinaryOperation::SSub || op == ar::BinaryOperation::USub)) ||
                (name == "and" && (op == ar::BinaryOperation::SAnd || op == ar::BinaryOperation::UAnd)) ||
                (name == "or" && (op == ar::BinaryOperation::SOr || op == ar::BinaryOperation::UOr)) ||
                (name == "xor" && (op == ar::BinaryOperation::SXor || op == ar::BinaryOperation::UXor)))) return false;
          stored = b->result();
        }
        auto* w = core::dyn_cast_or_null< ar::Store >(member());
        if (!store_ok(w, l->operand()) || (stored ? w->value() != stored : !native_sc_operand(w->value(), d.argument))) return false;
      }
    }
    for (auto j = i; j < cursor; ++j) heads[j] = i;
    groups.push_back({i, cursor, d}); i = cursor;
  }
  return true;
}
// Resolve only an actual native CAS extractvalue through its two checked inserts.
inline ar::Value* native_sc_extract(ar::ExtractElement* x,
    const std::vector< BmcEvent >& path, std::size_t before, std::size_t& value_before) {
  auto* raw = llvm::dyn_cast_or_null< llvm::ExtractValueInst >(native_sc_source(x));
  auto* cx = raw ? llvm::dyn_cast< llvm::AtomicCmpXchgInst >(raw->getAggregateOperand()) : nullptr;
  auto* off = x ? core::dyn_cast< ar::IntegerConstant >(x->offset()) : nullptr;
  if (!cx || raw->getNumIndices() != 1 || *raw->idx_begin() > 1 || !off) return nullptr;
  auto wanted = cx->getModule()->getDataLayout().getStructLayout(llvm::cast< llvm::StructType >(cx->getType()))
      ->getElementOffset(*raw->idx_begin());
  auto* integer = llvm::dyn_cast< llvm::IntegerType >(cx->getCompareOperand()->getType());
  if (!integer || off->value().to_z_number() != wanted ||
      !native_sc_integer(x->result(), *raw->idx_begin() == 0 ? integer->getBitWidth() : 1))
    return nullptr;
  ar::Value* v = x->aggregate();
  for (unsigned n = 0; n != 2; ++n) {
    auto* iv = core::dyn_cast< ar::InternalVariable >(v); std::size_t at = 0;
    auto* def = iv ? core::dyn_cast_or_null< ar::InsertElement >(reaching_definition(iv, path, before, at)) : nullptr;
    auto* offset = def ? core::dyn_cast< ar::IntegerConstant >(def->offset()) : nullptr;
    if (!def || native_sc_source(def) != cx || !offset) return nullptr;
    if (offset->value().to_z_number() == wanted) {value_before = at; return def->element();}
    v = def->aggregate(); before = at;
  }
  return nullptr;
}

inline ar::Value* native_sc_bound_operand(llvm::Value* raw,
    const std::vector< BmcEvent >& path, std::size_t before) {
  if (!raw || before >= path.size()) return nullptr;
  auto* code = path[before].stmt->code();
  const auto frame = invocation_call(path, code, before);
  for (std::size_t i = before; i-- > 0;) {
    auto* stmt = path[i].stmt;
    if (stmt->code() == code && invocation_call(path, code, i) == frame &&
        stmt->has_result() && native_sc_operand(stmt->result(), raw)) return stmt->result();
  }
  auto* arg = llvm::dyn_cast< llvm::Argument >(raw);
  auto* fn = code->function_or_null();
  if (arg && fn && arg->getArgNo() < fn->num_parameters() && native_sc_operand(fn->param(arg->getArgNo()), raw))
    return fn->param(arg->getArgNo());
  return nullptr;
}
