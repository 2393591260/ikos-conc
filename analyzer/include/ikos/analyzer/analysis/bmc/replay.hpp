#pragma once
#include <ikos/analyzer/analysis/source_c_abi.hpp>

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
  std::map< llvm::BasicBlock*, std::string > _native_blocks;
  std::map< llvm::Value*, ar::Value* > _native_values;

  std::string variable_id(ar::Value* value) {
    return _variables.emplace(value, "v" + std::to_string(_variables.size())).first->second;
  }

public:
  explicit ReplaySnapshot(ar::Bundle* bundle) : _bundle(bundle) {
    for (auto it = bundle->global_begin(); it != bundle->global_end(); ++it)
      if ((*it)->has_frontend()) _native_values.emplace((*it)->frontend< llvm::Value >(), *it);
    for (auto it = bundle->function_begin(); it != bundle->function_end(); ++it) {
      if ((*it)->is_declaration()) continue;
      if ((*it)->has_frontend())
        for (auto& block : *(*it)->frontend< llvm::Function >())
          _native_blocks.emplace(&block, "n" + std::to_string(_native_blocks.size()));
      for (auto* block : *(*it)->body_or_null()) {
        _blocks.emplace(block, "b" + std::to_string(_blocks.size()));
        for (auto* stmt : *block) {
          _statements.emplace(stmt, "s" + std::to_string(_statements.size()));
          auto* variable = stmt->has_result() ? core::dyn_cast< ar::Variable >(stmt->result()) : nullptr;
          if (variable && variable->has_frontend())
            _native_values.emplace(variable->frontend< llvm::Value >(), variable);
        }
      }
    }
  }

  JsonDict type(ar::Type* t, unsigned depth = 0) {
    JsonDict out;
    if (depth >= 32 || t->is_opaque()) {out.put("kind", "unsupported"); return out;}
    if (t->is_void() || t->is_function()) {out.put("kind", "unsupported"); return out;}
    out.put("size", _bundle->data_layout().store_size_in_bytes(t).str());
    out.put("extent", _bundle->data_layout().alloc_size_in_bytes(t).str());
    out.put("abi_alignment", _bundle->data_layout().abi_alignment(t));
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
    out.put("store_size", layout.getTypeStoreSize(type).getFixedValue());
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
    if (stmt->has_result() && stmt->result()->type()->is_pointer() && stmt->has_frontend()) {
      auto* phi = llvm::dyn_cast< llvm::PHINode >(stmt->frontend< llvm::Value >());
      if (phi) {
        JsonList incoming;
        for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i) {
          auto it = _native_values.find(phi->getIncomingValue(i));
          incoming.add(JsonDict{{"block", _native_blocks.at(phi->getIncomingBlock(i))},
              {"value", it == _native_values.end() ? JsonDict{{"kind", "unsupported"}} : value(it->second)}});
        }
        out.put("native_pointer_phi", incoming);
      }
    }
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
      if (source) {
        out.put("native_owner", source->getFunction()->getName().str());
        out.put("native_alignment", source->getAlign().value());
        out.put("native_address_space", source->getAddressSpace());
        out.put("native_entry_allocation", source->getParent() == &source->getFunction()->getEntryBlock());
        if (auto* count = llvm::dyn_cast< llvm::ConstantInt >(source->getArraySize()))
          if (count->getBitWidth() <= 64) out.put("native_count", count->getZExtValue());
      }
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
      out.put("atomic", load->is_atomic()); out.put("volatile", load->is_volatile());
    } else if (auto* store = core::dyn_cast< ar::Store >(stmt)) {
      out.put("kind", "store"); out.put("alignment", store->alignment());
      out.put("atomic", store->is_atomic()); out.put("volatile", store->is_volatile());
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
          if (callee && (callee->getName() == "malloc" || callee->getName() == "calloc")) {
            JsonDict native;
            native.put("name", callee->getName().str());
            native.put("declaration", callee->isDeclaration());
            native.put("variadic", callee->isVarArg());
            native.put("calling_convention", callee->getCallingConv());
            native.put("matching_signature", call->getFunctionType() == callee->getFunctionType());
            native.put("operand_bundles", call->hasOperandBundles());
            JsonList parameters, arguments;
            for (auto* type : callee->getFunctionType()->params())
              parameters.add(type->isIntegerTy() ? type->getIntegerBitWidth() : 0);
            for (const auto& arg : call->args())
              arguments.add(arg->getType()->isIntegerTy() ? arg->getType()->getIntegerBitWidth() : 0);
            native.put("parameter_bits", parameters); native.put("argument_bits", arguments);
            if (auto* type = llvm::dyn_cast< llvm::PointerType >(callee->getReturnType()))
              native.put("return_address_space", type->getAddressSpace());
            if (auto* type = llvm::dyn_cast< llvm::PointerType >(call->getType()))
              native.put("result_address_space", type->getAddressSpace());
            if (callee->getName() == "calloc") {
              auto* imported = core::dyn_cast< ar::CallBase >(stmt);
              bool bound = imported && imported->num_arguments() == 2 && call->arg_size() == 2;
              JsonList actuals;
              if (bound) for (unsigned n = 0; n < 2; ++n) {
                bound &= source_integer_binding(imported->argument(n), call->getArgOperand(n));
                actuals.add(value(imported->argument(n)));
              }
              native.put("source_operands_bound", bound);
              native.put("argument_values", actuals);
              out.put("native_calloc", native);
            } else out.put("native_malloc", native);
          }
          if (callee && (callee->getName() == "sem_init" ||
                         callee->getName() == "sem_wait" ||
                         callee->getName() == "sem_post" ||
                         callee->getName() == "pthread_barrier_init" ||
                         callee->getName() == "pthread_barrier_wait")) {
            JsonDict native;
            native.put("name", callee->getName().str());
            native.put("declaration", callee->isDeclaration());
            native.put("variadic", callee->isVarArg());
            native.put("calling_convention", callee->getCallingConv());
            native.put("matching_signature", call->getFunctionType() == callee->getFunctionType());
            native.put("operand_bundles", call->hasOperandBundles());
            bool special = false;
            JsonList parameters, arguments;
            for (auto* type : callee->getFunctionType()->params())
              parameters.add(type->isIntegerTy() ? type->getIntegerBitWidth() : 0);
            for (unsigned i = 0; i < call->arg_size(); ++i) {
              auto* type = call->getArgOperand(i)->getType();
              arguments.add(type->isIntegerTy() ? type->getIntegerBitWidth() : 0);
              for (auto attr : {llvm::Attribute::ByVal, llvm::Attribute::StructRet,
                               llvm::Attribute::InAlloca, llvm::Attribute::Preallocated})
                special |= call->paramHasAttr(i, attr) ||
                    (i < callee->arg_size() && callee->hasParamAttribute(i, attr));
            }
            native.put("special_parameter_abi", special);
            const auto& module_layout = call->getModule()->getDataLayout();
            native.put("pointer_bits", module_layout.getPointerSizeInBits(0));
            native.put("little_endian", module_layout.isLittleEndian());
            native.put("target_triple", call->getModule()->getTargetTriple());
            native.put("parameter_bits", parameters);
            native.put("argument_bits", arguments);
            auto* parameter = callee->arg_empty() ? nullptr
                : llvm::dyn_cast< llvm::PointerType >(callee->getFunctionType()->getParamType(0));
            auto* argument = call->arg_empty() ? nullptr
                : llvm::dyn_cast< llvm::PointerType >(call->getArgOperand(0)->getType());
            if (parameter) native.put("parameter_address_space", parameter->getAddressSpace());
            if (argument) native.put("argument_address_space", argument->getAddressSpace());
            native.put("return_bits", callee->getReturnType()->isIntegerTy()
                ? callee->getReturnType()->getIntegerBitWidth() : 0);
            native.put("result_bits", call->getType()->isIntegerTy()
                ? call->getType()->getIntegerBitWidth() : 0);
            out.put(callee->getName().startswith("pthread_barrier_") ? "native_barrier" : "native_semaphore", native);
          }
           if (callee && callee->getName() == "pthread_once") {
             JsonDict native;
             native.put("declaration", callee->isDeclaration());
             native.put("variadic", callee->isVarArg());
             native.put("calling_convention", callee->getCallingConv());
             native.put("call_convention", call->getCallingConv());
             native.put("matching_signature", call->getFunctionType() == callee->getFunctionType());
             native.put("operand_bundles", call->hasOperandBundles());
             native.put("arguments", call->arg_size());
             native.put("parameters", callee->arg_size());
             native.put("return_bits", callee->getReturnType()->isIntegerTy() ? callee->getReturnType()->getIntegerBitWidth() : 0);
             native.put("result_bits", call->getType()->isIntegerTy() ? call->getType()->getIntegerBitWidth() : 0);
             bool special = false;
             for (unsigned i = 0; i < call->arg_size(); ++i)
               for (auto attr : {llvm::Attribute::ByVal, llvm::Attribute::StructRet,
                                llvm::Attribute::InAlloca, llvm::Attribute::Preallocated})
                 special |= call->paramHasAttr(i, attr) ||
                     (i < callee->arg_size() && callee->hasParamAttribute(i, attr));
             native.put("special_parameter_abi", special);
             if (callee->arg_size() == 2 && call->arg_size() == 2) {
               auto* pointer = llvm::dyn_cast< llvm::PointerType >(callee->getFunctionType()->getParamType(0));
               native.put("control_bits", pointer && !pointer->isOpaque() && pointer->getPointerElementType()->isIntegerTy()
                   ? pointer->getPointerElementType()->getIntegerBitWidth() : 0);
               native.put("control_address_space", pointer ? pointer->getAddressSpace() : 99);
               auto* callback_pointer = llvm::dyn_cast< llvm::PointerType >(callee->getFunctionType()->getParamType(1));
               auto* signature = callback_pointer && !callback_pointer->isOpaque()
                   ? llvm::dyn_cast< llvm::FunctionType >(callback_pointer->getPointerElementType()) : nullptr;
               native.put("callback_parameter_void", signature && signature->getReturnType()->isVoidTy());
               native.put("callback_parameter_count", signature ? signature->getNumParams() : 99);
               native.put("callback_parameter_variadic", !signature || signature->isVarArg());
               native.put("callback_parameter_address_space", callback_pointer ? callback_pointer->getAddressSpace() : 99);
               auto* callback = llvm::dyn_cast< llvm::Function >(call->getArgOperand(1)->stripPointerCasts());
               native.put("callback_signature_matches", callback && signature == callback->getFunctionType());
               if (callback) {
                 native.put("callback", callback->getName().str());
                 native.put("callback_declaration", callback->isDeclaration());
                 native.put("callback_void", callback->getReturnType()->isVoidTy());
                 native.put("callback_count", callback->arg_size());
                 native.put("callback_variadic", callback->isVarArg());
                 native.put("callback_convention", callback->getCallingConv());
               }
               auto* control = llvm::dyn_cast< llvm::GlobalVariable >(call->getArgOperand(0));
               if (control) native.put("control", control->getName().str());
             }
              auto* actual = core::dyn_cast< ar::CallBase >(stmt);
              auto* direct = actual && actual->num_arguments() == 2
                  ? core::dyn_cast< ar::GlobalVariable >(actual->argument(0)) : nullptr;
              if (actual && !once_control_closed(direct)) {
                auto family = once_control_family(actual);
                if (family.valid) {
                  JsonDict manifest; JsonList controls, slots, uses; JsonDict calls;
                  manifest.put("schema", 1);
                  for (auto* control : family.controls) controls.add(control->name());
                  for (auto* slot : family.slots) slots.add(value(slot));
                  for (auto* use : family.statements) uses.add(_statements.at(use));
                  for (const auto& entry : family.calls) {
                    JsonList choices;
                    for (auto* control : entry.second) choices.add(control->name());
                    calls.put(_statements.at(entry.first), choices);
                  }
                  manifest.put("objects", controls); manifest.put("slots", slots);
                  manifest.put("uses", uses); manifest.put("calls", calls);
                  native.put("finite_controls", manifest);
                }
              }
             out.put("native_once", native);
           }
           if (callee && (callee->getName().startswith("pthread_mutexattr_") ||
               callee->getName() == "pthread_mutex_init" ||
               callee->getName() == "pthread_mutex_lock" || callee->getName() == "pthread_mutex_unlock")) {
             JsonDict native; JsonList parameters, spaces;
             native.put("name", callee->getName().str());
             native.put("pointer_bits", call->getModule()->getDataLayout().getPointerSizeInBits(0));
             native.put("index_bits", call->getModule()->getDataLayout().getIndexSizeInBits(0));
             native.put("little_endian", call->getModule()->getDataLayout().isLittleEndian());
             native.put("declaration", callee->isDeclaration());
             native.put("variadic", callee->isVarArg());
             native.put("calling_convention", callee->getCallingConv());
             native.put("call_convention", call->getCallingConv());
             native.put("matching_signature", call->getFunctionType() == callee->getFunctionType());
             native.put("operand_bundles", call->hasOperandBundles());
             native.put("ordinary_abi", ordinary_source_call_abi_attributes(call, callee));
             native.put("arguments", call->arg_size()); native.put("parameters", callee->arg_size());
             native.put("return_bits", callee->getReturnType()->isIntegerTy() ? callee->getReturnType()->getIntegerBitWidth() : 0);
             native.put("result_bits", call->getType()->isIntegerTy() ? call->getType()->getIntegerBitWidth() : 0);
             for (auto* parameter : callee->getFunctionType()->params()) {
               parameters.add(parameter->isIntegerTy() ? parameter->getIntegerBitWidth() : 0);
               auto* pointer = llvm::dyn_cast< llvm::PointerType >(parameter);
               spaces.add(pointer ? pointer->getAddressSpace() : 99);
             }
             native.put("parameter_integer_bits", parameters); native.put("parameter_address_spaces", spaces);
             JsonList objects;
             for (const auto& argument : call->args()) {
               auto* original = argument.get()->stripPointerCasts();
               auto mapped = _native_values.find(original);
               if (llvm::isa< llvm::ConstantPointerNull >(original)) objects.add(JsonDict{{"kind", "null"}});
               else if (mapped != _native_values.end()) objects.add(value(mapped->second));
               else objects.add(JsonDict{{"kind", "unsupported"}});
             }
             native.put("argument_objects", objects);
             if (callee->getName() == "pthread_mutexattr_settype" && call->arg_size() == 2)
               if (auto* kind = llvm::dyn_cast< llvm::ConstantInt >(call->getArgOperand(1)))
                 if (kind->getBitWidth() == 32) native.put("settype_value", kind->getSExtValue());
             out.put("native_mutex", native);
           }
           if (callee && callee->getName() == "pthread_create") {
             JsonDict native;
             native.put("name", callee->getName().str());
             native.put("declaration", callee->isDeclaration());
             native.put("variadic", callee->isVarArg());
             native.put("call_convention", call->getCallingConv());
             native.put("calling_convention", callee->getCallingConv());
             native.put("matching_signature", call->getFunctionType() == callee->getFunctionType());
             native.put("operand_bundles", call->hasOperandBundles());
             native.put("arguments", call->arg_size()); native.put("parameters", callee->arg_size());
             native.put("result_bits", call->getType()->isIntegerTy() ? call->getType()->getIntegerBitWidth() : 0);
             native.put("return_bits", callee->getReturnType()->isIntegerTy() ? callee->getReturnType()->getIntegerBitWidth() : 0);
             auto* child = call->arg_size() == 4
                 ? llvm::dyn_cast< llvm::Function >(call->getArgOperand(2)->stripPointerCasts()) : nullptr;
             bool special = false;
             for (unsigned i = 0; i < call->arg_size(); ++i)
               for (auto attribute : {llvm::Attribute::ByVal, llvm::Attribute::StructRet,
                                      llvm::Attribute::InAlloca, llvm::Attribute::Preallocated})
                 special |= call->paramHasAttr(i, attribute) ||
                     (i < callee->arg_size() && callee->hasParamAttribute(i, attribute)) ||
                     (child && i < child->arg_size() && child->hasParamAttribute(i, attribute));
             native.put("special_parameter_abi", special);
             native.put("ordinary_abi", ordinary_source_call_abi_attributes(call, callee));
             native.put("child_ordinary_abi", child && ordinary_source_function_abi_attributes(child));
             auto byte_pointer = [](llvm::Type* type) {
               auto* pointer = llvm::dyn_cast< llvm::PointerType >(type);
               return pointer && !pointer->isOpaque() && pointer->getAddressSpace() == 0 &&
                      pointer->getPointerElementType()->isIntegerTy(8);
             };
             auto* entry_pointer = callee->arg_size() == 4
                 ? llvm::dyn_cast< llvm::PointerType >(callee->getFunctionType()->getParamType(2)) : nullptr;
             native.put("child_signature_matches", child && entry_pointer && !entry_pointer->isOpaque() &&
                 entry_pointer->getAddressSpace() == 0 && entry_pointer->getPointerElementType() == child->getFunctionType());
             if (child) {
               native.put("child", child->getName().str());
               native.put("child_declaration", child->isDeclaration());
               native.put("child_variadic", child->isVarArg());
               native.put("child_convention", child->getCallingConv());
               native.put("child_parameters", child->arg_size());
               native.put("child_byte_pointer_argument", child->arg_size() == 1 &&
                   byte_pointer(child->getFunctionType()->getParamType(0)));
               native.put("child_byte_pointer_return", byte_pointer(child->getReturnType()));
             }
             out.put("native_create", native);
           }
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
      if (f->has_frontend()) fn.put("native_void_return", f->frontend< llvm::Function >()->getReturnType()->isVoidTy());
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
          JsonDict exported_block{{"statements", statements}, {"successors", successors}};
          if (block->has_frontend()) {
            auto* native = llvm::dyn_cast< llvm::BasicBlock >(block->frontend< llvm::Value >());
            if (native && _native_blocks.count(native)) exported_block.put("native_block", _native_blocks.at(native));
          }
          blocks.put(_blocks.at(block), exported_block);
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
        if (auto* structure = llvm::dyn_cast< llvm::StructType >(source->getValueType())) {
          if (structure->hasName()) object.put("frontend_type_name", structure->getName().str());
          if (structure->hasName() && structure->getName() == "union.pthread_mutex_t" && structure->isSized()) {
            const auto& layout = source->getParent()->getDataLayout();
            object.put("native_mutex_object", JsonDict{
                {"extent", layout.getTypeAllocSize(structure).getFixedValue()},
                {"store_size", layout.getTypeStoreSize(structure).getFixedValue()},
                {"type_alignment", layout.getABITypeAlign(structure).value()},
                {"alignment", source->getAlign() ? source->getAlign()->value() : layout.getABITypeAlign(structure).value()},
                {"address_space", source->getAddressSpace()}, {"declaration", source->isDeclaration()},
                {"thread_local", source->isThreadLocal()}, {"constant", source->isConstant()},
                {"externally_initialized", source->isExternallyInitialized()},
                {"module_inline_asm", !source->getParent()->getModuleInlineAsm().empty()}});
          }
          if (structure->hasName() && (structure->getName() == "union.sem_t" || structure->getName() == "union.pthread_barrier_t") && structure->isSized()) {
            const auto& layout = source->getParent()->getDataLayout();
            object.put(structure->getName() == "union.sem_t" ? "native_semaphore" : "native_barrier", JsonDict{
                {"extent", layout.getTypeAllocSize(structure).getFixedValue()},
                {"store_size", layout.getTypeStoreSize(structure).getFixedValue()},
                {"type_alignment", layout.getABITypeAlign(structure).value()},
                {"alignment", source->getAlign() ? source->getAlign()->value()
                                                : layout.getABITypeAlign(structure).value()},
                {"address_space", source->getAddressSpace()},
                {"declaration", source->isDeclaration()},
                {"thread_local", source->isThreadLocal()},
                {"constant", source->isConstant()},
                {"externally_initialized", source->isExternallyInitialized()}});
          }
        }
      }
      if (global->has_frontend()) {
        auto* source = global->frontend< llvm::GlobalVariable >();
        if (source->getValueType()->isIntegerTy(32)) {
          JsonDict native;
          auto* init = source->hasInitializer() ? llvm::dyn_cast< llvm::ConstantInt >(source->getInitializer()) : nullptr;
          native.put("zero_initializer", init && init->isZero());
          native.put("value_bits", 32);
          native.put("address_space", source->getAddressSpace());
          native.put("constant", source->isConstant());
          native.put("externally_initialized", source->isExternallyInitialized());
          native.put("module_inline_asm", !source->getParent()->getModuleInlineAsm().empty());
          native.put("module_aliases", !source->getParent()->alias_empty());
          native.put("constructors", source->getParent()->getGlobalVariable("llvm.global_ctors") != nullptr);
          native.put("destructors", source->getParent()->getGlobalVariable("llvm.global_dtors") != nullptr);
          JsonList uses; bool direct = true;
          for (auto* user : source->users()) {
            auto* call = llvm::dyn_cast< llvm::CallBase >(user);
            auto* callee = call ? call->getCalledFunction() : nullptr;
            if (!callee || callee->getName() != "pthread_once" || call->arg_size() != 2 ||
                call->getArgOperand(0) != source) { direct = false; continue; }
            unsigned matches = 0;
            for (const auto& entry : _statements)
              if (entry.first->has_frontend() && entry.first->frontend< llvm::Value >() == call) {
                uses.add(entry.second); ++matches;
              }
            if (matches != 1) direct = false;
          }
          native.put("all_uses_direct", direct); native.put("uses", uses);
          object.put("native_once_control", native);
        }
      }
      object.put("declaration", global->is_declaration()); JsonList stores;
      if (global->has_frontend() && global->type()->pointee()->is_pointer()) {
        auto* source = global->frontend< llvm::GlobalVariable >();
        if (source->hasInitializer())
          if (auto* target = llvm::dyn_cast< llvm::Function >(source->getInitializer()->stripPointerCasts()))
            object.put("frontend_function_initializer", target->getName().str());
      }
      if (global->has_frontend() && global->type()->pointee()->is_pointer()) {
        auto* source = global->frontend< llvm::GlobalVariable >();
        auto* pointer = llvm::dyn_cast< llvm::PointerType >(source->getValueType());
        auto* target = source->hasInitializer()
            ? llvm::dyn_cast< llvm::GlobalVariable >(source->getInitializer()) : nullptr;
        if (pointer && target)
          object.put("native_pointer_initializer", JsonDict{
              {"target", target->getName().str()}, {"address_space", source->getAddressSpace()},
              {"pointer_address_space", pointer->getAddressSpace()},
              {"pointer_bits", source->getParent()->getDataLayout().getPointerSizeInBits(pointer->getAddressSpace())},
              {"thread_local", source->isThreadLocal()}, {"declaration", source->isDeclaration()},
              {"externally_initialized", source->isExternallyInitialized()}});
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
