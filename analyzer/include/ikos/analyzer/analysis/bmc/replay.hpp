#pragma once

#include <llvm/IR/GetElementPtrTypeIterator.h>
#include <map>
#include <sstream>
#include <ikos/analyzer/json/json.hpp>
#include <ikos/analyzer/util/source_location.hpp>
#include "unroll.hpp"

namespace ikos { namespace analyzer { namespace bmc {

// This is an AR snapshot exporter, not a validator. No BMC transfer/solver rule
// is used to generate the instruction semantics or the complete function CFGs.
class ReplaySnapshot {
  ar::Bundle* _bundle;
  std::map< ar::Value*, std::string > _variables;
  std::map< ar::Statement*, std::string > _statements;
  std::map< ar::BasicBlock*, std::string > _blocks;

  std::string variable_id(ar::Value* value) {
    return _variables.emplace(value, "v" + std::to_string(_variables.size())).first->second;
  }

public:
  explicit ReplaySnapshot(ar::Bundle* bundle) : _bundle(bundle) {
    for (auto it = bundle->function_begin(); it != bundle->function_end(); ++it) {
      if ((*it)->is_declaration()) continue;
      for (auto* block : *(*it)->body_or_null()) {
        _blocks.emplace(block, "b" + std::to_string(_blocks.size()));
        for (auto* stmt : *block)
          _statements.emplace(stmt, "s" + std::to_string(_statements.size()));
      }
    }
  }

  JsonDict type(ar::Type* t, unsigned depth = 0) {
    JsonDict out;
    if (depth >= 32 || t->is_opaque()) {out.put("kind", "unsupported"); return out;}
    if (t->is_void() || t->is_function()) {out.put("kind", "unsupported"); return out;}
    out.put("size", _bundle->data_layout().store_size_in_bytes(t).str());
    out.put("extent", _bundle->data_layout().alloc_size_in_bytes(t).str());
    if (auto* i = core::dyn_cast< ar::IntegerType >(t)) {
      out.put("kind", "integer"); out.put("bits", i->bit_width()); out.put("signed", i->is_signed());
    } else if (core::isa< ar::PointerType >(t)) {
      out.put("kind", "pointer"); out.put("bits", _bundle->data_layout().pointers.bit_width);
      auto* pointee = core::cast< ar::PointerType >(t)->pointee();
      if (auto* integer = core::dyn_cast< ar::IntegerType >(pointee))
        out.put("pointee", type(integer, depth + 1));
    } else if (auto* a = core::dyn_cast< ar::ArrayType >(t)) {
      out.put("kind", "array"); out.put("count", a->num_elements().str());
      out.put("element", type(a->element_type(), depth + 1));
    } else if (auto* s = core::dyn_cast< ar::StructType >(t)) {
      out.put("kind", "struct"); JsonList fields;
      for (const auto& field : s->layout())
        fields.add(JsonDict{{"offset", field.offset.str()}, {"type", type(field.type, depth + 1)}});
      out.put("fields", fields);
    } else out.put("kind", "unsupported");
    return out;
  }

  JsonDict value(ar::Value* v, unsigned depth = 0) {
    JsonDict out; out.put("type", type(v->type()));
    if (depth >= 32) {out.put("kind", "unsupported"); return out;}
    if (auto* c = core::dyn_cast< ar::IntegerConstant >(v)) {
      out.put("kind", "integer"); out.put("value", c->value().to_z_number().str());
    } else if (auto* g = core::dyn_cast< ar::GlobalVariable >(v)) {
      out.put("kind", "global"); out.put("id", g->name());
    } else if (auto* f = core::dyn_cast< ar::FunctionPointerConstant >(v)) {
      out.put("kind", "function"); out.put("id", f->function()->name());
    } else if (core::isa< ar::InternalVariable >(v) || core::isa< ar::LocalVariable >(v)) {
      out.put("kind", core::isa< ar::LocalVariable >(v) ? "local" : "variable");
      out.put("id", variable_id(v));
    } else if (core::isa< ar::UndefinedConstant >(v)) out.put("kind", "undef");
    else if (core::isa< ar::NullConstant >(v)) out.put("kind", "null");
    else if (core::isa< ar::AggregateZeroConstant >(v)) out.put("kind", "zero");
    else if (auto* a = core::dyn_cast< ar::SequentialConstant >(v)) {
      out.put("kind", "sequence"); JsonList elements;
      if (a->num_elements() > 4096) out.put("kind", "unsupported");
      else for (auto* element : a->values()) elements.add(value(element, depth + 1));
      out.put("values", elements);
    } else if (auto* s = core::dyn_cast< ar::StructConstant >(v)) {
      out.put("kind", "struct"); JsonList fields;
      for (const auto& field : s->values())
        fields.add(JsonDict{{"offset", field.offset.str()}, {"value", value(field.value, depth + 1)}});
      out.put("fields", fields);
    } else out.put("kind", "unsupported");
    return out;
  }

  JsonDict native_type(llvm::Type* type, const llvm::DataLayout& layout,
                       unsigned& remaining, unsigned depth = 0) {
    JsonDict out;
    if (!remaining || depth >= 32 || !type->isSized()) {
      out.put("kind", "unsupported"); return out;
    }
    --remaining;
    const auto extent = layout.getTypeAllocSize(type);
    if (extent.isScalable()) {out.put("kind", "unsupported"); return out;}
    out.put("extent", extent.getFixedValue());
    out.put("alignment", layout.getABITypeAlign(type).value());
    if (auto* record = llvm::dyn_cast< llvm::StructType >(type)) {
      out.put("kind", "struct");
      if (record->hasName()) out.put("name", record->getName().str());
      JsonList fields;
      const auto* offsets = layout.getStructLayout(record);
      for (unsigned i = 0; i < record->getNumElements(); ++i)
        fields.add(JsonDict{{"offset", offsets->getElementOffset(i)},
            {"type", native_type(record->getElementType(i), layout, remaining, depth + 1)}});
      out.put("fields", fields);
    } else if (auto* array = llvm::dyn_cast< llvm::ArrayType >(type)) {
      out.put("kind", "array"); out.put("count", array->getNumElements());
      out.put("element", native_type(array->getElementType(), layout, remaining, depth + 1));
    } else {
      out.put("kind", "scalar");
      if (type->isIntegerTy()) out.put("bits", type->getIntegerBitWidth());
    }
    return out;
  }

  JsonDict statement(ar::Statement* stmt) {
    JsonDict out;
    out.put("abstract_initializer", abstract_initializer(stmt));
    // Structured opcodes/operands carry semantics. Text dumps recurse without
    // a bound through legal recursive C types, including unused declarations.
    out.put("block", _blocks.at(stmt->parent()));
    if (auto loc = source_location(stmt)) {
      out.put("file", loc.path().string()); out.put("line", loc.line()); out.put("column", loc.column());
    }
    if (stmt->has_result()) out.put("result", value(stmt->result()));
    JsonList operands;
    for (auto it = stmt->op_begin(); it != stmt->op_end(); ++it) operands.add(value(*it));
    out.put("operands", operands);
    if (core::isa< ar::Assignment >(stmt)) out.put("kind", "assign");
    else if (auto* un = core::dyn_cast< ar::UnaryOperation >(stmt)) {
      out.put("kind", "unary"); out.put("operator", static_cast< unsigned >(un->op()));
    } else if (auto* bin = core::dyn_cast< ar::BinaryOperation >(stmt)) {
      out.put("kind", "binary"); out.put("operator", static_cast< unsigned >(bin->op()));
      out.put("no_wrap", bin->has_no_wrap()); out.put("exact", bin->is_exact());
    } else if (auto* cmp = core::dyn_cast< ar::Comparison >(stmt)) {
      out.put("kind", "compare"); out.put("predicate", static_cast< unsigned >(cmp->predicate()));
    } else if (auto* shift = core::dyn_cast< ar::PointerShift >(stmt)) {
      out.put("kind", "pointer_shift"); JsonList factors;
      for (std::size_t i = 0; i < shift->num_terms(); ++i) factors.add(shift->term(i).first.to_z_number().str());
      out.put("factors", factors);
      auto* native = shift->has_frontend()
          ? llvm::dyn_cast< llvm::GetElementPtrInst >(shift->frontend< llvm::Value >()) : nullptr;
      if (native != nullptr) {
        out.put("inbounds", native->isInBounds());
        out.put("address_space", native->getPointerAddressSpace());
        out.put("index_bits", native->getModule()->getDataLayout().getIndexSizeInBits(native->getPointerAddressSpace()));
        JsonList terms;
        const auto& layout = native->getModule()->getDataLayout();
        for (auto it = llvm::gep_type_begin(native), end = llvm::gep_type_end(native); it != end; ++it) {
          JsonDict term;
          if (auto* record = it.getStructTypeOrNull()) {
            term.put("kind", "field");
            term.put("offset", layout.getStructLayout(record)->getElementOffset(
                llvm::cast< llvm::ConstantInt >(it.getOperand())->getZExtValue()));
          } else {
            const auto stride = layout.getTypeAllocSize(it.getIndexedType());
            term.put("kind", stride.isScalable() ? "unsupported" : "index");
            if (!stride.isScalable()) term.put("stride", stride.getFixedValue());
            term.put("bits", it.getOperand()->getType()->getIntegerBitWidth());
          }
          terms.add(term);
        }
        out.put("native_terms", terms);
      }
    } else if (auto* alloc = core::dyn_cast< ar::Allocate >(stmt)) {
      out.put("kind", "allocate"); out.put("allocated_type", type(alloc->allocated_type()));
      out.put("alignment", alloc->result()->alignment());
      auto* source = alloc->has_frontend()
          ? llvm::dyn_cast< llvm::AllocaInst >(alloc->frontend< llvm::Value >()) : nullptr;
      if (source && source->getAllocatedType()->isAggregateType()) {
        unsigned remaining = 512;
        out.put("native_layout", native_type(source->getAllocatedType(),
            source->getModule()->getDataLayout(), remaining));
        out.put("native_owner", source->getFunction()->getName().str());
        out.put("native_alignment", source->getAlign().value());
        out.put("native_address_space", source->getAddressSpace());
      }
    } else if (auto* load = core::dyn_cast< ar::Load >(stmt)) {
      out.put("kind", "load"); out.put("alignment", load->alignment());
      out.put("atomic", load->is_atomic());
    } else if (auto* store = core::dyn_cast< ar::Store >(stmt)) {
      out.put("kind", "store"); out.put("alignment", store->alignment());
      out.put("atomic", store->is_atomic());
    } else if (core::isa< ar::ReturnValue >(stmt)) out.put("kind", "return");
    else if (core::isa< ar::CallBase >(stmt)) {
      out.put("kind", "call"); out.put("abstract_initializer", abstract_initializer(stmt));
      if (stmt->has_frontend()) {
        auto* call = llvm::dyn_cast< llvm::CallBase >(stmt->frontend< llvm::Value >());
        if (call != nullptr) {
          auto* callee = call->getCalledFunction();
          if (callee && callee->getIntrinsicID() == llvm::Intrinsic::stacksave)
            out.put("native_intrinsic", "llvm.stacksave");
          if (auto* fill = llvm::dyn_cast< llvm::MemSetInst >(call)) {
            out.put("native_intrinsic", "llvm.memset");
            JsonDict native;
            native.put("volatile", fill->isVolatile());
            native.put("alignment", fill->getDestAlign().valueOrOne().value());
            native.put("address_space", fill->getDestAddressSpace());
            if (auto* byte = llvm::dyn_cast< llvm::ConstantInt >(fill->getValue()))
              native.put("byte", byte->getZExtValue());
            if (auto* length = llvm::dyn_cast< llvm::ConstantInt >(fill->getLength()))
              if (length->getValue().getActiveBits() <= 32)
                native.put("length", length->getZExtValue());
            auto* dest = fill->getDest()->stripPointerCasts();
            for (const auto& entry : _statements)
              if (auto* allocation = core::dyn_cast< ar::Allocate >(entry.first))
                if (allocation->has_frontend() &&
                    allocation->frontend< llvm::Value >() == dest)
                  native.put("destination", value(allocation->result()));
            out.put("native_memset", native);
          }
          out.put("calling_convention", call->getCallingConv());
          JsonList layouts;
          const auto& layout = call->getModule()->getDataLayout();
          for (const auto& argument : call->args()) {
            JsonDict item;
            auto* pointer = llvm::dyn_cast< llvm::PointerType >(argument->getType());
            auto* object = pointer != nullptr ? pointer->getPointerElementType() : nullptr;
            if (object != nullptr && object->isSized()) {
              item.put("size", layout.getTypeAllocSize(object).getFixedValue());
              item.put("alignment", layout.getABITypeAlign(object).value());
              if (auto* structure = llvm::dyn_cast< llvm::StructType >(object))
                if (structure->hasName()) item.put("name", structure->getName().str());
            }
            layouts.add(item);
          }
          out.put("argument_layouts", layouts);
        }
      }
    } else out.put("kind", "unsupported");
    return out;
  }

  JsonDict program() {
    JsonDict out, functions, instructions, globals;
    out.put("little_endian", _bundle->data_layout().is_little_endian());
    out.put("pointer_bits", _bundle->data_layout().pointers.bit_width);
    bool target_exported = false;
    for (auto it = _bundle->function_begin(); it != _bundle->function_end(); ++it) {
      auto* f = *it; JsonDict fn;
      if (!target_exported && f->has_frontend())
        if (auto* source = f->frontend< llvm::Function >())
          { out.put("target_triple", source->getParent()->getTargetTriple()); target_exported = true; }
      fn.put("declaration", f->is_declaration()); fn.put("return_type", type(f->type()->return_type()));
      fn.put("variadic", f->type()->is_var_arg());
      fn.put("intrinsic_nondet", f->intrinsic_id() == ar::Intrinsic::IkosNonDet);
      fn.put("intrinsic_assume", f->intrinsic_id() == ar::Intrinsic::IkosAssume);
      JsonList parameter_types;
      for (auto p = f->type()->param_begin(); p != f->type()->param_end(); ++p) parameter_types.add(type(*p));
      fn.put("parameter_types", parameter_types);
      if (f->is_definition()) {
        JsonList parameters; for (auto p = f->param_begin(); p != f->param_end(); ++p) parameters.add(value(*p));
        fn.put("parameters", parameters);
        fn.put("entry", _blocks.at(f->body_or_null()->entry_block())); JsonDict blocks;
        for (auto* block : *f->body_or_null()) {
          JsonList statements, successors;
          for (auto* s : *block) {statements.add(_statements.at(s)); instructions.put(_statements.at(s), statement(s));}
          for (auto s = block->successor_begin(); s != block->successor_end(); ++s) successors.add(_blocks.at(*s));
          blocks.put(_blocks.at(block), JsonDict{{"statements", statements}, {"successors", successors}});
        }
        fn.put("blocks", blocks);
      }
      functions.put(f->name(), fn);
    }
    for (auto it = _bundle->global_begin(); it != _bundle->global_end(); ++it) {
      auto* global = *it; JsonDict object;
      object.put("type", type(global->type()->pointee())); object.put("thread_local", global->is_thread_local());
      object.put("alignment", global->alignment());
      if (global->has_frontend()) {
        auto* source = global->frontend< llvm::GlobalVariable >();
        if (auto* structure = llvm::dyn_cast< llvm::StructType >(source->getValueType()))
          if (structure->hasName()) object.put("frontend_type_name", structure->getName().str());
      }
      object.put("declaration", global->is_declaration()); JsonList stores;
      if (global->has_frontend() && global->type()->pointee()->is_pointer()) {
        auto* source = global->frontend< llvm::GlobalVariable >();
        if (source->hasInitializer())
          if (auto* target = llvm::dyn_cast< llvm::Function >(source->getInitializer()->stripPointerCasts()))
            object.put("frontend_function_initializer", target->getName().str());
      }
      bool simple = true;
      if (global->is_definition()) for (auto* bb : *global->initializer()) for (auto* stmt : *bb) {
        auto* store = core::dyn_cast< ar::Store >(stmt);
        if (store != nullptr && store->pointer() == global) stores.add(value(store->value()));
        else simple = false;
      }
      object.put("initializers", stores); object.put("simple_initializer", simple);
      globals.put(global->name(), object);
    }
    out.put("functions", functions); out.put("instructions", instructions); out.put("globals", globals);
    return out;
  }

  std::string statement_id(ar::Statement* stmt) const {return _statements.at(stmt);}
};

} } }
