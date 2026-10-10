#pragma once

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

  JsonDict statement(ar::Statement* stmt) {
    JsonDict out;
    out.put("abstract_initializer", abstract_initializer(stmt));
    std::ostringstream text; stmt->dump(text); out.put("ar", text.str());
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
    } else if (auto* alloc = core::dyn_cast< ar::Allocate >(stmt)) {
      out.put("kind", "allocate"); out.put("allocated_type", type(alloc->allocated_type()));
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
