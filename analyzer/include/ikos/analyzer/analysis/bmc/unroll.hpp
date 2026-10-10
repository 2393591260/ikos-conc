// Bounded, path-sensitive prefixes to a candidate access. Calls compose their
// actual return prefixes; loop guards remain explicit and backedges are cut.
// Unsupported prefix semantics leave the candidate unconfirmed.

#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>
#include <functional>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Module.h>

#include "diagnostics.hpp"

#include <ikos/ar/semantic/code.hpp>
#include <ikos/ar/semantic/statement.hpp>
#include <ikos/core/support/cast.hpp>

namespace ikos {
namespace analyzer {
namespace bmc {

/// 事件种类。
enum class EvKind { Read, Write, Lock, Unlock, Assume, Create, Join, Local, Unsupported };

/// __VERIFIER_atomic_begin/end 的合成全局锁 id（与 abstract checker 的
/// PSEUDO_ATOMIC_LOCK 同值）。编码还须排除原子段外的其他线程事件。
constexpr std::uint64_t PSEUDO_ATOMIC_LOCK = 0xA7E0AD1CULL;

/// 一个 BMC 事件（内存访问或锁同步）。
struct BmcEvent {
  const char* blocker = nullptr;
  ar::Statement* stmt = nullptr;  // 对应的 AR 语句
  ar::Function* called_function = nullptr; // Callee proven on this executed call.
  bool synthetic_atomic = false; // Boundary marker, not another source call.
  bool synthetic_condition = false; // Wait's release/reacquire, not another call.
  bool candidate = false; // Selected dynamic access; atomic closure may follow.
  EvKind kind = EvKind::Read;
  std::uint64_t mutex = 0;  // Lock/Unlock 的锁对象语法同一性（arg0 指针）；内存事件为 0
  bool reader = false; // Two rwlock read acquisitions do not exclude each other.
  // 符号地址（SMT 同址门）：base = 全局数组指针身份；const_off = 常量字节偏移；
  // var_id/coeff = 单个变量下标项 `coeff * var`（var_id==0 ⟹ 纯常量偏移）。
  // 提取失败时保持 base==0（调用方保守不确认）。
  ar::LocalVariable* local_base = nullptr;
  ar::CallBase* heap_allocation = nullptr; // One executed allocation in implicit main.
  core::ZNumber heap_extent{0};
  std::uint64_t base = 0;
  core::ZNumber const_off{0};
  std::uint64_t var_id = 0;
  std::size_t var_before = 0; // Definition context of the pointer's index.
  core::ZNumber coeff{0};
  // 目标访问的字节宽度（encode.hpp 用范围重叠判同址，Read/Write 事件才有意义）。
  // Set for every memory event; confirmation requires known object bounds.
  std::uint64_t width = 0;
  // Create/Join 的线程身份：Create = 线程函数指针（reinterpret_cast）；Join =
  // 句柄身份（mutex_id_of_value 同款解析）。仅 Create/Join 事件用，其余为 0。
  std::uint64_t thread = 0;

  BmcEvent() = default;
  BmcEvent(ar::Statement* s, EvKind k, std::uint64_t m = 0)
      : stmt(s), kind(k), mutex(m) {}
};

// A unique static definition is usable only when its execution is also
// established by the selected path (encode.hpp).
inline ar::Statement* unique_def_bmc(ar::InternalVariable* iv) {
  ar::Code* code = iv->code();
  if (code == nullptr) {
    return nullptr;
  }
  ar::Statement* found = nullptr;
  for (ar::BasicBlock* bb : *code) {
    for (ar::Statement* s : *bb) {
      if (s->result_or_null() == iv) {
        if (found != nullptr) {
          return nullptr;  // phi：多个 def，当作叶子
        }
        found = s;
      }
    }
  }
  return found;
}

// Multiple definitions need actual reaching-definition evidence, not the first
// block found anywhere on the DFS stack.
inline ar::Statement* unique_def_bmc_on_path(
    ar::InternalVariable* iv, const std::vector< ar::BasicBlock* >& in_stack) {
  ar::Statement* found = nullptr;
  if (iv->code() == nullptr) return nullptr;
  for (ar::BasicBlock* bb : in_stack) {
    for (ar::Statement* s : *bb) {
      if (s->result_or_null() == iv) {
        if (found != nullptr) return nullptr;
        found = s;
      }
    }
  }
  return found;
}

// Only exact global addresses establish lock identity. An unresolved pointer
// cannot be assigned a distinct synthetic lock: it may alias the other lock.
inline std::uint64_t mutex_id_of_value(
    ar::Value* v, const std::vector< ar::BasicBlock* >* in_stack = nullptr) {
  core::ZNumber offset{0};
  for (unsigned depth = 0; depth < 16; ++depth) {
    if (auto* global = core::dyn_cast< ar::GlobalVariable >(v)) {
      if (!global->is_definition() || global->type()->pointee()->is_opaque()) return 0;
      auto size = global->bundle()->data_layout().alloc_size_in_bytes(global->type()->pointee());
      if (offset < 0 || offset >= size) return 0;
      auto base = reinterpret_cast< std::uint64_t >(global);
      // A collision can only add exclusion; incomplete ownership is rejected.
      std::uint64_t off = offset.to< std::uint64_t >();
      return base ^ (off + 0x9e3779b97f4a7c15ULL + (base << 6) + (base >> 2));
    }
    auto* iv = core::dyn_cast< ar::InternalVariable >(v);
    if (iv == nullptr) return 0;
    auto* def = in_stack != nullptr ? unique_def_bmc_on_path(iv, *in_stack) : unique_def_bmc(iv);
    if (auto* shift = core::dyn_cast_or_null< ar::PointerShift >(def)) {
      for (std::size_t i = 0; i < shift->num_terms(); ++i) {
        auto term = shift->term(i);
        auto* c = core::dyn_cast< ar::IntegerConstant >(term.second);
        if (c == nullptr) return 0;
        offset += term.first.to_z_number() * c->value().to_z_number();
      }
      v = shift->pointer();
    } else if (auto* cast = core::dyn_cast_or_null< ar::UnaryOperation >(def)) {
      if (cast->op() != ar::UnaryOperation::Bitcast) return 0;
      v = cast->operand();
    } else {
      return 0;
    }
  }
  return 0;
}

// A points-to MAY set does not prove a particular indirect callee executes.
inline ar::Function* exact_callee(ar::CallBase* call) {
  ar::Value* value = call->called();
  for (unsigned depth = 0; depth < 16; ++depth) {
    if (auto* f = core::dyn_cast< ar::FunctionPointerConstant >(value)) return f->function();
    auto* iv = core::dyn_cast< ar::InternalVariable >(value);
    if (iv == nullptr) return nullptr;
    auto* def = unique_def_bmc(iv);
    if (auto* cast = core::dyn_cast_or_null< ar::UnaryOperation >(def)) {
      if (cast->op() != ar::UnaryOperation::Bitcast) return nullptr;
      value = cast->operand();
    } else if (auto* assignment = core::dyn_cast_or_null< ar::Assignment >(def)) {
      value = assignment->operand();
    } else {
      return nullptr;
    }
  }
  return nullptr;
}

/// pthread_mutex_lock/unlock 的锁身份 = arg0。
inline std::uint64_t mutex_id(
    ar::CallBase* call, const std::vector< ar::BasicBlock* >* in_stack = nullptr) {
  if (call->arg_begin() == call->arg_end()) {
    return 0;
  }
  return mutex_id_of_value(*call->arg_begin(), in_stack);
}

/// 判断一个 CallBase 是否是对 pthread_mutex 的 lock/unlock，返回事件种类；
/// 非锁调用返回 false。
inline bool classify_mutex_call(ar::CallBase* call, EvKind& kind) {
  ar::Function* f = exact_callee(call);
  if (f == nullptr || !f->is_declaration()) return false;
  const auto& name = f->name();
  if (f->intrinsic_id() == ar::Intrinsic::PthreadMutexLock || name == "pthread_mutex_lock" ||
      (name == "pthread_rwlock_rdlock" && call->num_arguments() == 1) ||
      (name == "pthread_rwlock_wrlock" && call->num_arguments() == 1) ||
      name == "__VERIFIER_atomic_begin") {
    kind = EvKind::Lock;
    return true;
  }
  if (f->intrinsic_id() == ar::Intrinsic::PthreadMutexUnlock || name == "pthread_mutex_unlock" ||
      (name == "pthread_rwlock_unlock" && call->num_arguments() == 1) ||
      name == "__VERIFIER_atomic_end") {
    kind = EvKind::Unlock;
    return true;
  }
  return false;
}

/// 判断一个 CallBase 是否是对 __VERIFIER_atomic_begin/end 的调用。
inline bool is_verifier_atomic(ar::CallBase* call) {
  auto* f = exact_callee(call);
  return f != nullptr && f->is_declaration() &&
         (f->name() == "__VERIFIER_atomic_begin" || f->name() == "__VERIFIER_atomic_end");
}

inline bool named_verifier_atomic(ar::Function* function) {
  return function != nullptr && function->is_definition() &&
         function->name().compare(0, 18, "__VERIFIER_atomic_") == 0;
}

/// 提取一次内存访问指针的符号字节偏移：沿 def-chain 穿透 PointerShift / 值保持
/// cast，累加常量项（stride*const）与至多一个变量下标项（stride*var）。下标只认
/// 直接 InternalVariable；仿射（i+1）/多变量/phi 等不干净的形态返回 false（base
/// 保持 0，调用方保守不确认，只丢 TP 不引 FP）。
inline bool symbolic_byte_offset(ar::Value* ptr, BmcEvent& ev) {
  for (int depth = 0; depth < 16; ++depth) {
    if (auto* gv = core::dyn_cast< ar::GlobalVariable >(ptr)) {
      ev.base = reinterpret_cast< std::uint64_t >(gv);
      return true;
    }
    if (auto* local = core::dyn_cast< ar::LocalVariable >(ptr)) {
      ev.local_base = local;
      return true;
    }
    auto* iv = core::dyn_cast< ar::InternalVariable >(ptr);
    if (iv == nullptr) {
      return false;
    }
    ar::Statement* def = unique_def_bmc(iv);
    if (auto* ps = core::dyn_cast_or_null< ar::PointerShift >(def)) {
      for (std::size_t i = 0; i < ps->num_terms(); ++i) {
        auto term = ps->term(i);
        core::ZNumber stride = term.first.to_z_number();
        if (auto* ic = core::dyn_cast< ar::IntegerConstant >(term.second)) {
          ev.const_off += stride * ic->value().to_z_number();
        } else if (auto* viv =
                       core::dyn_cast< ar::InternalVariable >(term.second)) {
          if (ev.var_id != 0 &&
              ev.var_id != reinterpret_cast< std::uint64_t >(viv)) {
            return false; // 多变量下标：不干净
          }
          ev.var_id = reinterpret_cast< std::uint64_t >(viv);
          ev.coeff += stride;
        } else {
          return false;
        }
      }
      ptr = ps->pointer();
      continue;
    }
    if (auto* un = core::dyn_cast_or_null< ar::UnaryOperation >(def)) {
      switch (un->op()) {
        case ar::UnaryOperation::Bitcast:
          ptr = un->operand();
          continue;
        default:
          return false;
      }
    }
    return false;
  }
  return false;
}

// Known object extent for prefix validity. Locals require one bounded stack
// allocation; candidate objects still have to be globals.
inline bool memory_extent(const BmcEvent& event, core::ZNumber& size) {
  if (event.heap_allocation != nullptr) {
    size = event.heap_extent;
    return size > 0 && size <= 1048576;
  }
  if (event.base != 0) {
    auto* g = reinterpret_cast< ar::GlobalVariable* >(event.base);
    auto* type = g->type()->pointee();
    // A TLS declaration denotes one object per dynamic thread instance.
    if (!g->is_definition() || g->is_thread_local() || type->is_opaque()) return false;
    size = g->bundle()->data_layout().alloc_size_in_bytes(type);
    return true;
  }
  if (event.local_base == nullptr) return false;
  ar::Allocate* found = nullptr;
  for (ar::BasicBlock* bb : *event.local_base->function()->body_or_null()) {
    for (ar::Statement* s : *bb) {
      if (s->result_or_null() == event.local_base) {
        if (found != nullptr) return false;
        found = core::dyn_cast< ar::Allocate >(s);
        if (found == nullptr) return false;
      }
    }
  }
  if (found == nullptr || found->allocated_type()->is_opaque()) return false;
  auto* n = core::dyn_cast< ar::IntegerConstant >(found->array_size());
  if (n == nullptr || n->value().to_z_number() <= 0) return false;
  size = event.local_base->bundle()->data_layout().alloc_size_in_bytes(found->allocated_type()) * n->value().to_z_number();
  return size > 0 && size <= 1048576;
}

/// 判断一个 CallBase 是否是 assume 语义调用（assume_abort_if_not / __VERIFIER_assume）。
/// 它们「assume 实参为真」（假分支 abort/不可达），不能当普通调用跳过。
inline bool is_assume_call(ar::CallBase* call) {
  auto* f = exact_callee(call);
  return f != nullptr && f->is_declaration() &&
         (f->intrinsic_id() == ar::Intrinsic::IkosAssume || f->name() == "__VERIFIER_assume");
}

// A deliberately small stdio summary: an immutable, terminated literal and
// optionally promoted int conversions. String/pointer arguments, %n, dynamic
// width and return-value dependencies remain unsupported.
inline bool constant_output_call(ar::CallBase* call) {
  auto* function = exact_callee(call);
  if (function == nullptr || !function->is_declaration()) return false;
  auto name = function->name();
  bool puts = name == "puts" || name == "ar.libc.puts";
  bool printf = name == "printf" || name == "ar.libc.printf";
  if ((!puts && !printf) || call->num_arguments() == 0) return false;
  BmcEvent address;
  if (!symbolic_byte_offset(call->argument(0), address) || address.base == 0 || address.var_id != 0 || address.const_off < 0) return false;
  auto* global = reinterpret_cast< ar::GlobalVariable* >(address.base);
  if (!global->is_definition() || !global->has_frontend() ||
      !global->frontend< llvm::GlobalVariable >()->isConstant()) return false;
  ar::ArrayConstant* text = nullptr;
  for (auto* block : *global->initializer())
    for (auto* statement : *block) {
      auto* store = core::dyn_cast< ar::Store >(statement);
      if (store == nullptr || store->pointer() != global || text != nullptr) return false;
      text = core::dyn_cast< ar::ArrayConstant >(store->value());
    }
  if (text == nullptr || address.const_off >= text->values().size()) return false;
  std::string format;
  bool terminated = false;
  for (std::size_t i = address.const_off.to< std::size_t >(); i < text->values().size(); ++i) {
    auto* c = core::dyn_cast< ar::IntegerConstant >(text->values()[i]);
    if (c == nullptr || c->value().bit_width() != 8) return false;
    auto character = c->value().to_z_number();
    if (character == 0) {terminated = true; break;}
    if (character < 0 || character > 127) return false;
    format += static_cast< char >(character.to< unsigned >());
  }
  if (!terminated) return false;
  if (puts) return call->num_arguments() == 1;
  std::size_t argument = 1;
  for (std::size_t i = 0; i < format.size(); ++i) {
    if (format[i] != '%') continue;
    if (++i == format.size()) return false;
    if (format[i] == '%') continue;
    if (std::string("diuoxXc").find(format[i]) == std::string::npos || argument >= call->num_arguments()) return false;
    auto* type = core::dyn_cast< ar::IntegerType >(call->argument(argument++)->type());
    bool signed_conversion = format[i] == 'd' || format[i] == 'i' || format[i] == 'c';
    if (type == nullptr || type->bit_width() != 32 || type->is_signed() != signed_conversion) return false;
  }
  return argument == call->num_arguments();
}

inline bool malloc_call(ar::CallBase* call) {
  auto* function = exact_callee(call);
  auto* source = call->has_frontend()
      ? llvm::dyn_cast< llvm::CallBase >(call->frontend< llvm::Value >()) : nullptr;
  auto* declaration = source != nullptr
      ? llvm::dyn_cast< llvm::Function >(source->getCalledOperand()->stripPointerCasts()) : nullptr;
  const auto bits = call->bundle()->data_layout().pointers.bit_width;
  return function != nullptr && function->is_declaration() &&
         function->intrinsic_id() == ar::Intrinsic::LibcMalloc && call->num_arguments() == 1 &&
         declaration != nullptr && declaration->isDeclaration() && declaration->getName() == "malloc" &&
         !declaration->isVarArg() && declaration->arg_size() == 1 &&
         declaration->getReturnType()->isPointerTy() && source->getType()->isPointerTy() &&
         declaration->getFunctionType()->getParamType(0)->isIntegerTy(bits) &&
         source->arg_size() == 1 && source->getArgOperand(0)->getType()->isIntegerTy(bits);
}

// A direct worker termination endpoint; it is never an ordinary returning leaf.
// Callback registration and non-null return payloads remain unsupported.
inline bool thread_exit_call(ar::CallBase* call) {
  if (call == nullptr || !call->code()->is_function_body() ||
      call->code()->function()->name() == "main") return false;
  auto* function = exact_callee(call);
  auto* source = call->has_frontend()
      ? llvm::dyn_cast< llvm::CallBase >(call->frontend< llvm::Value >()) : nullptr;
  auto* declaration = source != nullptr
      ? llvm::dyn_cast< llvm::Function >(source->getCalledOperand()->stripPointerCasts()) : nullptr;
  return function != nullptr && function->is_declaration() && function->name() == "pthread_exit" &&
         function->type()->return_type()->is_void() && !call->has_result() && call->num_arguments() == 1 &&
         core::isa< ar::NullConstant >(call->argument(0)) && declaration != nullptr &&
         declaration->isDeclaration() && declaration->getName() == "pthread_exit" && declaration->doesNotReturn() &&
         llvm::isa< llvm::CallInst >(source) && source->getCallingConv() == llvm::CallingConv::C &&
         declaration->getCallingConv() == llvm::CallingConv::C &&
         !declaration->isVarArg() && declaration->arg_size() == 1 && declaration->getReturnType()->isVoidTy() &&
         declaration->getFunctionType()->getParamType(0)->isPointerTy() && source->getType()->isVoidTy() &&
         source->arg_size() == 1 && source->getArgOperand(0)->getType()->isPointerTy();
}

// AR synchronization intrinsics erase pointee types to i8*. Recover the
// original call's complete control-storage layout, including trailing padding.
inline bool synchronization_layout(ar::CallBase* call, unsigned argument,
                                   std::uint64_t& width, std::uint64_t& alignment) {
  auto* source = call->has_frontend()
      ? llvm::dyn_cast< llvm::CallBase >(call->frontend< llvm::Value >()) : nullptr;
  if (source == nullptr || argument >= source->arg_size()) return false;
  auto* pointer = llvm::dyn_cast< llvm::PointerType >(source->getArgOperand(argument)->getType());
  if (pointer == nullptr || pointer->isOpaque()) return false;
  auto* object = pointer->getPointerElementType();
  if (!object->isSized() || object->isIntegerTy(8)) return false;
  const auto& layout = source->getModule()->getDataLayout();
  auto size = layout.getTypeAllocSize(object);
  if (size.isScalable() || size.getFixedValue() == 0) return false;
  width = size.getFixedValue();
  alignment = layout.getABITypeAlign(object).value();
  return true;
}

// Attribute effects require an executed lifecycle and opaque-storage checks in
// the encoder. Matching an API name alone is not an initialization proof.
inline bool mutex_attribute_call(ar::CallBase* call) {
  auto* function = exact_callee(call);
  if (function == nullptr || !function->is_declaration()) return false;
  const auto& name = function->name();
  if (name != "pthread_mutexattr_init" && name != "pthread_mutexattr_settype" &&
      name != "pthread_mutexattr_destroy" && name != "pthread_mutex_init") return false;
  const unsigned count = name == "pthread_mutexattr_settype" || name == "pthread_mutex_init" ? 2 : 1;
  auto* returned = core::dyn_cast< ar::IntegerType >(function->type()->return_type());
  auto* source = call->has_frontend() ? llvm::dyn_cast< llvm::CallBase >(call->frontend< llvm::Value >()) : nullptr;
  auto* declaration = source != nullptr ? llvm::dyn_cast< llvm::Function >(source->getCalledOperand()->stripPointerCasts()) : nullptr;
  if (returned == nullptr || returned->bit_width() != 32 || !returned->is_signed() ||
      call->num_arguments() != count || declaration == nullptr || !declaration->isDeclaration() ||
      declaration->getName() != name || declaration->isVarArg() || declaration->arg_size() != count ||
      source->arg_size() != count || !declaration->getReturnType()->isIntegerTy(32) ||
      !source->getType()->isIntegerTy(32) || source->getCallingConv() != llvm::CallingConv::C ||
      declaration->getCallingConv() != llvm::CallingConv::C) return false;
  for (unsigned i = 0; i < count; ++i) {
    auto* formal = declaration->getFunctionType()->getParamType(i);
    auto* actual = source->getArgOperand(i)->getType();
    if (name == "pthread_mutexattr_settype" && i == 1) {
      if (!formal->isIntegerTy(32) || !actual->isIntegerTy(32)) return false;
    } else if (!formal->isPointerTy() || !actual->isPointerTy()) return false;
  }
  return true;
}

// Memory-using summaries must discharge their object/effect obligations later.
// Unknown externals have no such summary.
inline bool supported_leaf_call(ar::CallBase* call) {
  auto* f = exact_callee(call);
  if (f == nullptr || !f->is_declaration()) return false;
  if (constant_output_call(call)) return true;
  if (f->intrinsic_id() == ar::Intrinsic::IkosNonDet) return true;
  if (f->intrinsic_id() == ar::Intrinsic::LibcMalloc)
    return malloc_call(call) && call->code()->is_function_body() &&
           call->code()->function()->name() == "main";
  static const std::set< std::string > nondet{
      "__VERIFIER_nondet_bool", "__VERIFIER_nondet_char", "__VERIFIER_nondet_uchar",
      "__VERIFIER_nondet_short", "__VERIFIER_nondet_ushort", "__VERIFIER_nondet_int",
      "__VERIFIER_nondet_uint", "__VERIFIER_nondet_long", "__VERIFIER_nondet_ulong",
      "__VERIFIER_nondet_longlong", "__VERIFIER_nondet_ulonglong", "__VERIFIER_nondet_size_t"};
  if (nondet.count(f->name()) != 0 && call->num_arguments() == 0) return true;
  if (f->name() == "pthread_detach") return call->num_arguments() == 1;
  if (f->name() == "pthread_mutex_init") {
    return mutex_attribute_call(call);
  }
  if (f->name() == "pthread_mutexattr_init" || f->name() == "pthread_mutexattr_settype" ||
      f->name() == "pthread_mutexattr_destroy") return mutex_attribute_call(call);
  // Object initialization and ownership are checked on the actual prefixes.
  if (f->name() == "pthread_cond_init" || f->name() == "pthread_cond_wait" ||
      f->name() == "pthread_cond_signal" || f->name() == "pthread_cond_broadcast") {
    auto* returned = core::dyn_cast< ar::IntegerType >(f->type()->return_type());
    if (returned == nullptr || returned->bit_width() != 32 || !returned->is_signed()) return false;
    if (f->name() == "pthread_cond_init")
      return call->num_arguments() == 2 && core::isa< ar::NullConstant >(call->argument(1));
    return call->num_arguments() == (f->name() == "pthread_cond_wait" ? 2u : 1u);
  }
  return false;
}

// Supported local operations remain in the prefix. The encoder must discharge
// arithmetic definedness even when their results do not affect the target.
inline bool total_local_statement(ar::Statement* s) {
  if (auto* allocation = core::dyn_cast< ar::Allocate >(s)) {
    BmcEvent event;
    event.local_base = allocation->result();
    core::ZNumber size;
    return memory_extent(event, size);
  }
  if (core::isa< ar::Assignment >(s) || core::isa< ar::PointerShift >(s) ||
      core::isa< ar::ReturnValue >(s)) return true;
  if (auto* op = core::dyn_cast< ar::BinaryOperation >(s)) {
    switch (op->op()) {
      case ar::BinaryOperation::UAdd: case ar::BinaryOperation::SAdd:
      case ar::BinaryOperation::USub: case ar::BinaryOperation::SSub:
      case ar::BinaryOperation::UMul: case ar::BinaryOperation::SMul:
        return true;
      case ar::BinaryOperation::UAnd: case ar::BinaryOperation::SAnd:
      case ar::BinaryOperation::UOr: case ar::BinaryOperation::SOr:
      case ar::BinaryOperation::UXor: case ar::BinaryOperation::SXor:
      case ar::BinaryOperation::UDiv: case ar::BinaryOperation::SDiv:
      case ar::BinaryOperation::URem: case ar::BinaryOperation::SRem:
      case ar::BinaryOperation::UShl: case ar::BinaryOperation::SShl:
      case ar::BinaryOperation::ULShr: case ar::BinaryOperation::SLShr:
      case ar::BinaryOperation::UAShr: case ar::BinaryOperation::SAShr:
        return true;
      default: return false;
    }
  }
  auto* un = core::dyn_cast< ar::UnaryOperation >(s);
  if (un == nullptr) return false;
  switch (un->op()) {
    case ar::UnaryOperation::Bitcast:
    case ar::UnaryOperation::ZExt:
    case ar::UnaryOperation::SExt:
    case ar::UnaryOperation::UTrunc:
    case ar::UnaryOperation::STrunc:
      return true;
    default: return false;
  }
}

inline bool abstract_initializer(ar::Statement* s) {
  auto* source = s->has_frontend()
                     ? llvm::dyn_cast< llvm::Instruction >(s->frontend< llvm::Value >())
                     : nullptr;
  return source != nullptr && source->getMetadata("ikos.freeze_uninit") != nullptr;
}

inline bool source_nondet(ar::CallBase* call) {
  auto* f = exact_callee(call);
  auto* source = call->has_frontend()
                     ? llvm::dyn_cast< llvm::CallBase >(call->frontend< llvm::Value >())
                     : nullptr;
  auto* returned = f != nullptr ? core::dyn_cast< ar::IntegerType >(f->type()->return_type()) : nullptr;
  auto* result = call->result_or_null() != nullptr ? core::dyn_cast< ar::IntegerType >(call->result_or_null()->type()) : nullptr;
  return f != nullptr && f->num_parameters() == 0 && call->num_arguments() == 0 &&
         returned != nullptr && result != nullptr &&
         returned->bit_width() == result->bit_width() && returned->is_signed() == result->is_signed() &&
         supported_leaf_call(call) &&
         (f->intrinsic_id() == ar::Intrinsic::IkosNonDet ||
          f->name().compare(0, 18, "__VERIFIER_nondet_") == 0) &&
         source != nullptr && source->getDebugLoc() && !abstract_initializer(call);
}

// Dynamic invocation boundaries, reconstructed from the composed call/return
// prefix. Static AR variables are reused by separate helper invocations.
inline ar::Function* callee_on_path(ar::CallBase* call,
                                    const std::vector< BmcEvent >& path,
                                    std::size_t before);
inline std::size_t invocation_call(const std::vector< BmcEvent >& path,
                                   ar::Code* code, std::size_t before) {
  std::vector< std::pair< ar::Code*, std::size_t > > stack;
  for (std::size_t i = 0; i < before; ++i) {
    if (path[i].synthetic_atomic) continue;
    if (auto* call = core::dyn_cast< ar::CallBase >(path[i].stmt)) {
      auto* function = path[i].called_function != nullptr ? path[i].called_function : exact_callee(call);
      if (function != nullptr && function->is_definition()) stack.push_back({function->body_or_null(), i});
    } else if (core::isa< ar::ReturnValue >(path[i].stmt) && !stack.empty() && stack.back().first == path[i].stmt->code()) {
      stack.pop_back();
    }
  }
  for (auto i = stack.rbegin(); i != stack.rend(); ++i)
    if (i->first == code) return i->second;
  return path.size();
}

inline ar::Statement* reaching_definition(ar::InternalVariable* value,
                                          const std::vector< BmcEvent >& path,
                                          std::size_t before, std::size_t& index) {
  auto call = invocation_call(path, value->code(), before);
  std::size_t begin = call == path.size() ? 0 : call + 1;
  ar::Statement* definition = nullptr;
  for (std::size_t i = begin; i < before; ++i)
    if (!path[i].synthetic_atomic && !path[i].synthetic_condition && path[i].stmt->result_or_null() == value && invocation_call(path, value->code(), i) == call) {definition = path[i].stmt; index = i;}
  return definition;
}

inline ar::Value* formal_argument(ar::InternalVariable* value,
                                 const std::vector< BmcEvent >& path,
                                 std::size_t before, std::size_t& call_index) {
  if (value->code() == nullptr || !value->code()->is_function_body()) return nullptr;
  auto* function = value->code()->function();
  call_index = invocation_call(path, value->code(), before);
  if (call_index == path.size()) return nullptr;
  auto* call = core::cast< ar::CallBase >(path[call_index].stmt);
  for (std::size_t i = 0; i < function->num_parameters(); ++i)
    if (function->param(i) == value && i < call->num_arguments()) return call->argument(i);
  return nullptr;
}

// A callback passed by an executed call is concrete evidence. A points-to
// set or a pointer loaded from shared memory is not such evidence.
inline ar::Function* callee_on_path(ar::CallBase* call,
                                    const std::vector< BmcEvent >& path,
                                    std::size_t before) {
  if (auto* function = exact_callee(call)) return function;
  if (before < path.size() && path[before].stmt == call && path[before].called_function != nullptr)
    return path[before].called_function;
  ar::Value* value = call->called();
  for (unsigned depth = 0; depth < 32; ++depth) {
    if (auto* pointer = core::dyn_cast< ar::FunctionPointerConstant >(value)) return pointer->function();
    auto* variable = core::dyn_cast< ar::InternalVariable >(value);
    if (variable == nullptr) return nullptr;
    std::size_t index = 0;
    auto* definition = reaching_definition(variable, path, before, index);
    if (definition == nullptr) {
      value = formal_argument(variable, path, before, index);
      if (value == nullptr) return nullptr;
    } else if (auto* assignment = core::dyn_cast< ar::Assignment >(definition)) {
      value = assignment->operand();
    } else if (auto* cast = core::dyn_cast< ar::UnaryOperation >(definition)) {
      if (cast->op() != ar::UnaryOperation::Bitcast) return nullptr;
      value = cast->operand();
    } else return nullptr;
    before = index;
  }
  return nullptr;
}

inline bool root_parameter(ar::InternalVariable* value, ar::Function* root) {
  return root != nullptr && root->is_definition() && root->num_parameters() == 1 && root->param(0) == value;
}

inline ar::Value* created_argument(ar::Function* child,
                                   const std::vector< BmcEvent >& creator,
                                   unsigned instance = 0) {
  for (const auto& event : creator) {
    if (event.kind == EvKind::Create && event.thread == reinterpret_cast< std::uint64_t >(child)) {
      if (instance-- == 0) return core::cast< ar::CallBase >(event.stmt)->argument(3);
    }
  }
  return nullptr;
}

// Fold only executed, pure integer definitions with representable results.
// Symbolic/loaded indices still require SMT; overflow never proves an address.
inline bool constant_integer_on_path(ar::Value* value,
                                     const std::vector< BmcEvent >& path,
                                     std::size_t before, core::ZNumber& result,
                                     unsigned depth = 0) {
  if (depth >= 128) return false;
  if (auto* constant = core::dyn_cast< ar::IntegerConstant >(value)) {
    result = constant->value().to_z_number(); return true;
  }
  auto* variable = core::dyn_cast< ar::InternalVariable >(value);
  auto* type = variable == nullptr ? nullptr : core::dyn_cast< ar::IntegerType >(variable->type());
  if (type == nullptr) return false;
  std::size_t index = 0;
  auto* definition = reaching_definition(variable, path, before, index);
  if (definition == nullptr || abstract_initializer(definition)) return false;
  if (auto* assignment = core::dyn_cast< ar::Assignment >(definition)) {
    if (assignment->operand()->type() != variable->type() ||
        !constant_integer_on_path(assignment->operand(), path, index, result, depth + 1)) return false;
  } else if (auto* operation = core::dyn_cast< ar::BinaryOperation >(definition)) {
    core::ZNumber left, right;
    if (operation->left()->type() != variable->type() || operation->right()->type() != variable->type() ||
        !constant_integer_on_path(operation->left(), path, index, left, depth + 1) ||
        !constant_integer_on_path(operation->right(), path, index, right, depth + 1)) return false;
    switch (operation->op()) {
      case ar::BinaryOperation::SAdd: case ar::BinaryOperation::UAdd: result = left + right; break;
      case ar::BinaryOperation::SSub: case ar::BinaryOperation::USub: result = left - right; break;
      case ar::BinaryOperation::SMul: case ar::BinaryOperation::UMul: result = left * right; break;
      default: return false;
    }
  } else if (auto* cast = core::dyn_cast< ar::UnaryOperation >(definition)) {
    if (cast->op() != ar::UnaryOperation::SExt && cast->op() != ar::UnaryOperation::ZExt &&
        cast->op() != ar::UnaryOperation::Bitcast) return false;
    auto* source = core::dyn_cast< ar::IntegerType >(cast->operand()->type());
    if (source == nullptr || type->bit_width() < source->bit_width() ||
        !constant_integer_on_path(cast->operand(), path, index, result, depth + 1) ||
        (cast->op() == ar::UnaryOperation::Bitcast && type->bit_width() != source->bit_width()) ||
        (cast->op() == ar::UnaryOperation::ZExt && result < 0) ||
        (cast->op() == ar::UnaryOperation::SExt && !source->is_signed() &&
         result >= (core::ZNumber(1) << (source->bit_width() - 1)))) return false;
  } else return false;
  core::ZNumber lo = type->is_signed() ? -(core::ZNumber(1) << (type->bit_width() - 1)) : core::ZNumber(0);
  core::ZNumber hi = (core::ZNumber(1) << (type->is_signed() ? type->bit_width() - 1 : type->bit_width())) - 1;
  return result >= lo && result <= hi;
}

// Normalize x + c only through value-preserving assignments and no-wrap
// additions/subtractions. The leaf is one executed definition, never a load's
// assumed value; equal leaves cancel without knowing x.
struct AffineInteger {
  ar::InternalVariable* base = nullptr;
  std::size_t definition = 0;
  core::ZNumber offset{0};
};

inline bool affine_integer_on_path(ar::Value* value,
                                   const std::vector< BmcEvent >& path,
                                   std::size_t before, AffineInteger& result,
                                   unsigned depth = 0) {
  if (depth >= 128) return false;
  if (auto* constant = core::dyn_cast< ar::IntegerConstant >(value)) {
    result = {nullptr, 0, constant->value().to_z_number()}; return true;
  }
  auto* variable = core::dyn_cast< ar::InternalVariable >(value);
  if (variable == nullptr || !variable->type()->is_integer()) return false;
  std::size_t index = 0;
  auto* definition = reaching_definition(variable, path, before, index);
  if (definition == nullptr) {
    auto* actual = formal_argument(variable, path, before, index);
    return actual != nullptr && actual->type() == variable->type() &&
           affine_integer_on_path(actual, path, index, result, depth + 1);
  }
  if (abstract_initializer(definition)) return false;
  if (auto* assignment = core::dyn_cast< ar::Assignment >(definition)) {
    if (assignment->operand()->type() == variable->type())
      return affine_integer_on_path(assignment->operand(), path, index, result, depth + 1);
  }
  if (auto* operation = core::dyn_cast< ar::BinaryOperation >(definition)) {
    auto* type = core::cast< ar::IntegerType >(variable->type());
    bool signed_op = operation->op() == ar::BinaryOperation::SAdd || operation->op() == ar::BinaryOperation::SSub;
    if (operation->has_no_wrap() && type->is_signed() == signed_op && operation->left()->type() == variable->type() &&
        operation->right()->type() == variable->type()) {
      AffineInteger left, right;
      bool add = operation->op() == ar::BinaryOperation::SAdd || operation->op() == ar::BinaryOperation::UAdd;
      bool sub = operation->op() == ar::BinaryOperation::SSub || operation->op() == ar::BinaryOperation::USub;
      if ((add || sub) && affine_integer_on_path(operation->left(), path, index, left, depth + 1) &&
          affine_integer_on_path(operation->right(), path, index, right, depth + 1)) {
        if (right.base == nullptr) {
          result = left; result.offset += sub ? -right.offset : right.offset; return true;
        }
        if (add && left.base == nullptr) {
          result = right; result.offset += left.offset; return true;
        }
      }
    }
  }
  result = {variable, index, core::ZNumber(0)};
  return true;
}

// A successful, bounded malloc choice is usable only at one executed site in
// implicit main before any child starts. Repeated sites need dynamic identities.
inline bool main_allocation(ar::CallBase* call, const std::vector< BmcEvent >& path,
                            std::size_t index, core::ZNumber& size) {
  auto* function = exact_callee(call);
  if (function == nullptr || !malloc_call(call) ||
      !call->code()->is_function_body() || call->code()->function()->name() != "main" ||
      !call->has_result() || !call->result()->type()->is_pointer() ||
      index >= path.size() || path[index].stmt != call ||
      invocation_call(path, call->code(), index) != path.size() ||
      std::count_if(path.begin(), path.end(), [&](const auto& event) {return event.stmt == call;}) != 1)
    return false;
  for (std::size_t i = 0; i < index; ++i)
    if (path[i].kind == EvKind::Create) return false;
  return constant_integer_on_path(call->argument(0), path, index, size) && size > 0 && size <= 1048576;
}

// A pointer slot may be followed outside a worker only if its address never
// escapes and its sole source store actually ran in main before all creates.
inline ar::GlobalVariable* global_pointer_slot(ar::Value* value) {
  BmcEvent address;
  if (!symbolic_byte_offset(value, address) || address.base == 0 ||
      address.var_id != 0 || address.const_off != 0) return nullptr;
  auto* global = reinterpret_cast< ar::GlobalVariable* >(address.base);
  return global->type()->pointee()->is_pointer() ? global : nullptr;
}

inline ar::Store* published_pointer(ar::GlobalVariable* global,
                                    const std::vector< BmcEvent >& origin,
                                    std::size_t& index) {
  if (!global->is_definition() || global->is_thread_local() || !global->has_frontend()) return nullptr;
  auto* source = global->frontend< llvm::GlobalVariable >();
  llvm::StoreInst* initialization = nullptr;
  for (auto* user : source->users()) {
    if (auto* load = llvm::dyn_cast< llvm::LoadInst >(user)) {
      if (load->getPointerOperand() != source || !load->getType()->isPointerTy()) return nullptr;
    } else if (auto* store = llvm::dyn_cast< llvm::StoreInst >(user)) {
      if (store->getPointerOperand() != source || initialization != nullptr ||
          store->getFunction()->getName() != "main") return nullptr;
      initialization = store;
    } else return nullptr;
  }
  if (initialization == nullptr) return nullptr;
  ar::Store* found = nullptr;
  bool spawned = false;
  for (std::size_t i = 0; i < origin.size(); ++i) {
    spawned |= origin[i].kind == EvKind::Create;
    auto* store = core::dyn_cast< ar::Store >(origin[i].stmt);
    if (store == nullptr || !store->has_frontend() ||
        store->frontend< llvm::Value >() != initialization) continue;
    if (spawned || found != nullptr || global_pointer_slot(store->pointer()) != global ||
        invocation_call(origin, store->code(), i) != origin.size()) return nullptr;
    found = store; index = i;
  }
  return found;
}

// Resolve pointers from executed definitions and actual helper/thread args.
// Root arguments were consumed on the already checked creation prefix; only
// constant global address chains may be followed outside the selected path.
inline bool pointer_on_path(ar::Value* value, const std::vector< BmcEvent >& path,
                             std::size_t before, ar::Function* root,
                             ar::Value* root_argument, BmcEvent& event,
                             const std::vector< BmcEvent >* origins = nullptr) {
  if (origins == nullptr && root != nullptr && root->name() == "main") origins = &path;
  const auto* definitions = &path;
  bool external = false;
  bool returned = false;
  for (unsigned depth = 0; depth < 32; ++depth) {
    if (auto* global = core::dyn_cast< ar::GlobalVariable >(value)) {event.base = reinterpret_cast< std::uint64_t >(global); return true;}
    if (auto* local = core::dyn_cast< ar::LocalVariable >(value)) {
      if (external || returned) return false; // returned stack lifetime is unproved
      event.local_base = local; return true;
    }
    auto* variable = core::dyn_cast< ar::InternalVariable >(value);
    if (variable == nullptr) return false;
    std::size_t index = 0;
    std::size_t use = before;
    auto* definition = external && origins == nullptr ? unique_def_bmc(variable)
        : reaching_definition(variable, *definitions, before, index);
    if (definition == nullptr && !external) {
      auto* argument = formal_argument(variable, path, before, index);
      if (argument != nullptr) {value = argument; before = index; continue;}
      if (root_parameter(variable, root) && root_argument != nullptr) {
        value = root_argument; external = true;
        if (origins != nullptr) {definitions = origins; before = origins->size();}
        continue;
      }
      return false;
    }
    if (definition == nullptr) return false;
    if (!external || origins != nullptr) before = index;
    if (auto* shift = core::dyn_cast< ar::PointerShift >(definition)) {
      for (std::size_t i = 0; i < shift->num_terms(); ++i) {
        auto term = shift->term(i);
        if (auto* c = core::dyn_cast< ar::IntegerConstant >(term.second)) event.const_off += term.first.to_z_number() * c->value().to_z_number();
        else if (auto* index_var = core::dyn_cast< ar::InternalVariable >(term.second)) {
          core::ZNumber constant;
          if (!external && constant_integer_on_path(index_var, path, before, constant)) {
            event.const_off += term.first.to_z_number() * constant;
            continue;
          }
          auto identity = reinterpret_cast< std::uint64_t >(index_var);
          if (external || (event.var_id != 0 && (event.var_id != identity || event.var_before != before))) return false;
          event.var_id = identity; event.var_before = before; event.coeff += term.first.to_z_number();
        } else return false;
      }
      value = shift->pointer();
    } else if (auto* cast = core::dyn_cast< ar::UnaryOperation >(definition)) {
      if (cast->op() != ar::UnaryOperation::Bitcast) return false;
      value = cast->operand();
    } else if (auto* assignment = core::dyn_cast< ar::Assignment >(definition)) {
      value = assignment->operand();
    } else if (auto* load = core::dyn_cast< ar::Load >(definition)) {
      auto* slot = global_pointer_slot(load->operand());
      std::size_t store_index = 0;
      auto* store = slot != nullptr && origins != nullptr ? published_pointer(slot, *origins, store_index) : nullptr;
      if (store == nullptr || !load->result()->type()->is_pointer() ||
          (definitions == origins && store_index >= index)) return false;
      value = store->value(); external = true; definitions = origins; before = store_index;
    } else if (auto* call = core::dyn_cast< ar::CallBase >(definition)) {
      core::ZNumber size;
      if (main_allocation(call, *definitions, index, size)) {
        event.heap_allocation = call; event.heap_extent = size; return true;
      }
      if (external) return false;
      auto* callee = callee_on_path(call, path, index);
      if (callee == nullptr || callee->body_or_null() == nullptr) return false;
      ar::ReturnValue* result = nullptr;
      for (std::size_t j = index + 1; j < use; ++j) {
        auto* ret = core::dyn_cast< ar::ReturnValue >(path[j].stmt);
        if (ret != nullptr && ret->code() == callee->body_or_null() &&
            invocation_call(path, ret->code(), j) == index) {
          result = ret; before = j; break;
        }
      }
      if (result == nullptr || !result->has_operand() ||
          !result->operand()->type()->is_pointer()) return false;
      value = result->operand(); returned = true;
    } else return false;
  }
  return false;
}

// Create events choose a successful default-attribute spawn. The prefix checker
// separately proves its output slot valid; the corresponding POSIX int result is 0.
inline bool successful_create_result(const BmcEvent& event) {
  if (event.kind != EvKind::Create) return false;
  auto* call = core::dyn_cast< ar::CallBase >(event.stmt);
  auto* f = call != nullptr ? exact_callee(call) : nullptr;
  auto* result = call != nullptr && call->result_or_null() != nullptr
                     ? core::dyn_cast< ar::IntegerType >(call->result_or_null()->type()) : nullptr;
  auto* returned = f != nullptr ? core::dyn_cast< ar::IntegerType >(f->type()->return_type()) : nullptr;
  return f != nullptr && f->is_declaration() && call->num_arguments() == 4 &&
         (f->name() == "pthread_create" || f->intrinsic_id() == ar::Intrinsic::PthreadCreate) &&
         core::isa< ar::NullConstant >(call->argument(1)) && event.thread != 0 &&
         result != nullptr && returned != nullptr && result->bit_width() == 32 &&
         result->is_signed() && returned->bit_width() == 32 && returned->is_signed();
}

inline bool condition_call(ar::CallBase* call) {
  auto* function = exact_callee(call);
  if (function == nullptr || !function->is_declaration()) return false;
  const auto& name = function->name();
  return (name == "pthread_cond_init" || name == "pthread_cond_wait" ||
          name == "pthread_cond_signal" || name == "pthread_cond_broadcast") &&
         supported_leaf_call(call);
}

inline bool successful_condition_result(const BmcEvent& event) {
  auto* call = core::dyn_cast< ar::CallBase >(event.stmt);
  if (call == nullptr || !condition_call(call)) return false;
  auto* result = core::dyn_cast_or_null< ar::IntegerType >(
      call->has_result() ? call->result()->type() : nullptr);
  auto* returned = core::dyn_cast< ar::IntegerType >(exact_callee(call)->type()->return_type());
  return result != nullptr && returned != nullptr && result->bit_width() == 32 &&
         result->is_signed() && returned->bit_width() == 32 && returned->is_signed();
}

// Definedness is required even for values not used in guards/addresses.
// This does not encode load values or establish read-from relations.
inline bool prefix_values_defined(const std::vector< BmcEvent >& path,
                                  Diagnostics* diagnostics = nullptr,
                                  ar::Function* root = nullptr,
                                  ar::Value* root_argument = nullptr) {
  std::function< bool(ar::Value*, std::size_t, unsigned) > defined;
  defined = [&](ar::Value* value, std::size_t before, unsigned depth) {
    if (depth >= 32 || core::isa< ar::UndefinedConstant >(value)) return false;
    if (auto* aggregate = core::dyn_cast< ar::StructConstant >(value)) {
      for (const auto& field : aggregate->values())
        if (!defined(field.value, before, depth + 1)) return false;
    } else if (auto* aggregate = core::dyn_cast< ar::SequentialConstant >(value)) {
      for (auto* element : aggregate->values())
        if (!defined(element, before, depth + 1)) return false;
    }
    if (core::isa< ar::Constant >(value) || core::isa< ar::GlobalVariable >(value)) return true;
    std::size_t index = 0;
    ar::Statement* def = nullptr;
    if (auto* variable = core::dyn_cast< ar::InternalVariable >(value)) {
      def = reaching_definition(variable, path, before, index);
      if (def == nullptr) {
        auto* actual = formal_argument(variable, path, before, index);
        if (actual != nullptr) return defined(actual, index, depth + 1);
        if (root_parameter(variable, root) && root_argument != nullptr) {
          if (core::isa< ar::Constant >(root_argument) || core::isa< ar::GlobalVariable >(root_argument))
            return !core::isa< ar::UndefinedConstant >(root_argument);
          BmcEvent address;
          return pointer_on_path(variable, path, before, root, root_argument, address) && address.base != 0 && address.var_id == 0;
        }
      }
    } else {
      for (std::size_t i = 0; i < before; ++i)
        if (path[i].stmt->result_or_null() == value) {def = path[i].stmt; index = i;}
    }
    if (def == nullptr || abstract_initializer(def)) return false;
    if (core::isa< ar::Allocate >(def)) return true;
    if (core::isa< ar::Load >(def)) {
      const auto& access = path[index];
      // The encoder requires SC sources for every heap read, even unused ones.
      if (access.base != 0 || access.heap_allocation != nullptr) return true;
      if (access.local_base == nullptr || access.var_id != 0) return false;
      for (std::size_t j = index; j-- > 0;) {
        const auto& previous = path[j];
        // A prior invocation's store cannot initialize a new stack object.
        if (previous.stmt->result_or_null() == access.local_base) return false;
        if (previous.kind == EvKind::Create) {
          auto* call = core::cast< ar::CallBase >(previous.stmt);
          BmcEvent output;
          if (symbolic_byte_offset(call->argument(0), output) &&
              output.local_base == access.local_base && output.var_id == 0 &&
              output.const_off == access.const_off &&
              access.width <= call->bundle()->data_layout().store_size_in_bytes(
                  core::cast< ar::PointerType >(call->argument(0)->type())->pointee()).to< std::uint64_t >()) return true;
        }
        if (previous.kind != EvKind::Write || previous.local_base != access.local_base) continue;
        if (previous.var_id != 0) return false;
        if (previous.const_off >= access.const_off + access.width ||
            access.const_off >= previous.const_off + previous.width) continue;
        if (abstract_initializer(previous.stmt) ||
            previous.const_off > access.const_off ||
            previous.const_off + previous.width < access.const_off + access.width) return false;
        return defined(core::cast< ar::Store >(previous.stmt)->value(), j, depth + 1);
      }
      return false;
    }
    if (auto* call = core::dyn_cast< ar::CallBase >(def)) {
      core::ZNumber size;
      if (main_allocation(call, path, index, size)) return true;
      if (source_nondet(call)) return true;
      if (successful_create_result(path[index])) return true;
      if (successful_condition_result(path[index])) return true;
      auto* f = callee_on_path(call, path, index);
      if (f == nullptr || f->body_or_null() == nullptr) return false;
      for (std::size_t j = index + 1; j < before; ++j) {
        auto* ret = core::dyn_cast< ar::ReturnValue >(path[j].stmt);
        if (ret != nullptr && ret->code() == f->body_or_null() &&
            invocation_call(path, ret->code(), j) == index) {
          return ret->has_operand() && defined(ret->operand(), j, depth + 1);
        }
      }
      return false;
    }
    for (auto it = def->op_begin(); it != def->op_end(); ++it)
      if (!defined(*it, index, depth + 1)) return false;
    return true;
  };
  for (std::size_t i = 0; i < path.size(); ++i) {
    if (path[i].synthetic_atomic || path[i].synthetic_condition) continue;
    auto* stmt = path[i].stmt;
    // These two marked operations are abstract bookkeeping, not source uses.
    if (abstract_initializer(stmt)) continue;
    if (path[i].kind == EvKind::Create) {
      auto* call = core::cast< ar::CallBase >(stmt);
      BmcEvent output;
      core::ZNumber extent;
      auto* pointer = core::dyn_cast< ar::PointerType >(call->argument(0)->type());
      // Choose a successful default-attribute spawn only with a valid output
      // object. Null/unknown handles cannot establish a live child.
      if (pointer == nullptr || !core::isa< ar::IntegerType >(pointer->pointee()) ||
          !core::isa< ar::NullConstant >(call->argument(1)) ||
          !pointer_on_path(call->argument(0), path, i, root, root_argument, output) || output.var_id != 0 ||
          !memory_extent(output, extent) || output.const_off < 0 ||
          output.const_off + call->bundle()->data_layout().store_size_in_bytes(pointer->pointee()) > extent) {
        record(diagnostics, "create_arguments", stmt);
        return false;
      }
    }
    for (auto it = stmt->op_begin(); it != stmt->op_end(); ++it) {
      if (!defined(*it, i, 0)) {
        record(diagnostics, "prefix_value_origin", stmt);
        return false;
      }
    }
  }
  return true;
}

/// 把一个语句分类成 BMC 事件（内存/锁/假设/无关）。
inline bool classify_event(
    ar::Statement* stmt, BmcEvent& ev,
    const std::vector< ar::BasicBlock* >* in_stack = nullptr) {
  if (auto* load = core::dyn_cast< ar::Load >(stmt)) {
    ev = BmcEvent{stmt, EvKind::Read};
    if (load->is_atomic()) {ev.kind = EvKind::Unsupported; ev.blocker = "unmodeled_sync"; return true;}
    symbolic_byte_offset(load->operand(), ev);
    ev.width = stmt->parent()->bundle()->data_layout().store_size_in_bytes(load->result()->type()).to< std::uint64_t >();
    return true;
  }
  if (auto* store = core::dyn_cast< ar::Store >(stmt)) {
    ev = BmcEvent{stmt, EvKind::Write};
    if (store->is_atomic()) {ev.kind = EvKind::Unsupported; ev.blocker = "unmodeled_sync"; return true;}
    symbolic_byte_offset(store->pointer(), ev);
    ev.width = stmt->parent()->bundle()->data_layout().store_size_in_bytes(store->value()->type()).to< std::uint64_t >();
    return true;
  }
  if (core::isa< ar::Comparison >(stmt)) {
    // 路径条件（assume 约束）：比较在 AR 里就是 refine 语句，路径上每条
    // Comparison 都成立，收集起来在 encode 阶段编码成 SMT 约束。
    ev = BmcEvent{stmt, EvKind::Assume};
    return true;
  }
  if (auto* call = core::dyn_cast< ar::CallBase >(stmt)) {
    EvKind kind;
    if (classify_mutex_call(call, kind)) {
      std::uint64_t mid =
          is_verifier_atomic(call) ? PSEUDO_ATOMIC_LOCK : mutex_id(call, in_stack);
      ev = BmcEvent{stmt, kind, mid};
      auto* lock_function = exact_callee(call);
      ev.reader = lock_function != nullptr && lock_function->name() == "pthread_rwlock_rdlock";
      return true;
    }
    // pthread_create/pthread_join → Create/Join 事件（spawn/join HB 锚点）。
    ar::Function* f = exact_callee(call);
    if (f != nullptr && f->is_declaration()) {
      if ((f->name() == "pthread_create" || f->intrinsic_id() == ar::Intrinsic::PthreadCreate) && call->num_arguments() == 4) {
        // arg2 = 线程函数（FunctionPointerConstant）。函数指针变量/间接线程函数
        // 提取失败 → 不产生 Create 事件（保守，只丢 TP）。
        auto it = call->arg_begin();
        ++it;
        ++it;
        if (it != call->arg_end()) {
          if (auto* fpc = core::dyn_cast< ar::FunctionPointerConstant >(*it)) {
            if (fpc->function() != nullptr) {
              ev = BmcEvent{stmt, EvKind::Create};
              ev.thread = reinterpret_cast< std::uint64_t >(fpc->function());
              return true;
            }
          }
        }
      } else if (f->name() == "pthread_join" || f->intrinsic_id() == ar::Intrinsic::PthreadJoin) {
        // arg0 = pthread_t 句柄（mutex_id_of_value 同款解析到稳定身份）。
        if (call->arg_begin() != call->arg_end()) {
          ev = BmcEvent{stmt, EvKind::Join};
          ev.thread = mutex_id_of_value(*call->arg_begin(), in_stack);
          return true;
        }
      }
    }
  }
  return false;
}

/// 内联展开的最大调用深度（helper 内 helper 的递归上限，防爆炸）。
constexpr int MAX_INLINE_DEPTH = 8;

/// Hardware atomic builtins need summaries; verifier atomic definitions compose
/// their actual CFG with explicit indivisible-region boundaries.
inline bool bmc_skip_inline(const std::string& nm) {
  if (nm.size() >= 9 && nm.compare(0, 9, "__atomic_") == 0) {
    return true;
  }
  if (nm.size() >= 7 && nm.compare(0, 7, "__sync_") == 0) {
    return true;
  }
  return false;
}

/// 函数体是否含回边（循环）。helper 带循环时展开器（DAG-only）不能精确展开，
/// 跟进只会得到不完整路径 → 跳过不内联（保守 UNKNOWN）。
inline bool has_loop_dfs(ar::BasicBlock* bb,
                         std::vector< ar::BasicBlock* >& on_path,
                         std::vector< ar::BasicBlock* >& visited) {
  if (on_path.size() >= 256 || visited.size() >= 16384) return true;
  for (ar::BasicBlock* b : on_path) {
    if (b == bb) {
      return true;  // 回边
    }
  }
  for (ar::BasicBlock* b : visited) {
    if (b == bb) {
      return false;
    }
  }
  visited.push_back(bb);
  on_path.push_back(bb);
  for (auto succ = bb->successor_begin(), succ_end = bb->successor_end();
       succ != succ_end; ++succ) {
    if (has_loop_dfs(*succ, on_path, visited)) {
      on_path.pop_back();
      return true;
    }
  }
  on_path.pop_back();
  return false;
}

inline bool function_has_loop(ar::Function* f) {
  ar::Code* body = f->body_or_null();
  if (body == nullptr || !body->has_entry_block()) {
    return false;
  }
  std::vector< ar::BasicBlock* > on_path, visited;
  return has_loop_dfs(body->entry_block(), on_path, visited);
}

// Inline acyclic definitions; synchronization inside them remains explicit.
inline bool inline_ok(ar::Function* f) {
  if (f == nullptr || f->body_or_null() == nullptr) {
    return false;  // extern（pthread_* / 无体）
  }
  if (bmc_skip_inline(f->name())) {
    return false;  // 原子单调用 / assume
  }
  return true;
}

// The original LLVM signature/ABI, not an AR opaque-pointer type, establishes
// whether the selected body can implement this indirect invocation.
inline bool compatible_indirect_callee(ar::CallBase* call, ar::Function* target) {
  if (!call->has_frontend() || target == nullptr || !target->has_frontend() || !inline_ok(target)) return false;
  auto* source = llvm::dyn_cast< llvm::CallBase >(call->frontend< llvm::Value >());
  auto* function = target->frontend< llvm::Function >();
  return source != nullptr && source->getCallingConv() == llvm::CallingConv::C &&
         function->getCallingConv() == llvm::CallingConv::C && !function->isVarArg() &&
         source->getFunctionType() == function->getFunctionType() &&
         source->arg_size() == call->num_arguments() && target->num_parameters() == call->num_arguments();
}

inline std::vector< ar::Function* > indirect_targets(ar::CallBase* call) {
  std::vector< ar::Function* > targets;
  for (auto i = call->bundle()->function_begin(); i != call->bundle()->function_end(); ++i) {
    auto* target = *i;
    if (!compatible_indirect_callee(call, target)) continue;
    auto* source = target->frontend< llvm::Function >();
    bool addressed = false;
    for (auto* user : source->users()) {
      auto* direct = llvm::dyn_cast< llvm::CallBase >(user);
      addressed |= direct == nullptr || direct->getCalledOperand()->stripPointerCasts() != source;
    }
    if (addressed) targets.push_back(target);
  }
  std::sort(targets.begin(), targets.end(), [](auto* a, auto* b) {return a->name() < b->name();});
  return targets;
}

/// 路径敏感的 DFS：从 `bb` 出发，沿 CFG 后继枚举到 `target` 的所有路径。
/// `current` retains the executed prefix. A selected atomic access continues
/// through its real closing boundary; Return searches retain the actual suffix.
/// `in_stack` 记录当前 DFS 栈上的基本块，用于检测回边（循环）并跳过——
/// Backedges are cut; retained prefixes must still satisfy their actual guards.
constexpr std::size_t MAX_PATHS = 256;
constexpr std::size_t MAX_PATH_EVENTS = 1024;
struct UnrollBudget {
  bool prune_join_prefix = false;
  std::size_t steps = 0;
  unsigned loop_bound = 1;
  unsigned target_occurrence = 1;
  ar::Statement* root_target = nullptr;
  bool completed = false;
  const char* exhausted = nullptr;
  const char* rejected_reason = nullptr;
  ar::Statement* rejected_statement = nullptr;
};

inline bool open_atomic_prefix(const std::vector< BmcEvent >& path) {
  int active = 0;
  for (const auto& event : path) {
    if (event.mutex != PSEUDO_ATOMIC_LOCK) continue;
    if (event.kind == EvKind::Lock) ++active;
    if (event.kind == EvKind::Unlock) --active;
  }
  return active != 0;
}

inline std::size_t candidate_index(const std::vector< BmcEvent >& path) {
  for (std::size_t i = 0; i < path.size(); ++i)
    if (path[i].candidate) return i;
  return path.size();
}

void dfs_paths(ar::BasicBlock* bb, ar::Statement* target,
               std::vector< BmcEvent >& current,
               std::vector< std::vector< BmcEvent > >& out,
               std::vector< ar::BasicBlock* > in_stack,
               UnrollBudget& budget, int depth = 0, ar::Statement* resume_after = nullptr) {
  if (budget.exhausted != nullptr) {
    return;
  }
  if (++budget.steps > 16384 || in_stack.size() >= 256) {
    budget.exhausted = "unroll_work_budget";
    return;
  }
  if (out.size() >= MAX_PATHS) {
    budget.exhausted = "unroll_path_budget";
    return;
  }
  bool on_stack = false;
  if (std::count(in_stack.begin(), in_stack.end(), bb) >= budget.loop_bound) return;

  if (!on_stack) {
    in_stack.push_back(bb);
  }

  if (!on_stack) {
    bool resumed = resume_after == nullptr;
    for (auto sit = bb->begin(), send = bb->end(); sit != send; ++sit) {
      ar::Statement* s = *sit;
      if (!resumed) {
        if (s == resume_after) resumed = true;
        continue;
      }
      if (auto* comparison = core::dyn_cast< ar::Comparison >(s)) {
        core::ZNumber left, right;
        auto* type = core::dyn_cast< ar::IntegerType >(comparison->left()->type());
        AffineInteger affine_left, affine_right;
        bool known = constant_integer_on_path(comparison->left(), current, current.size(), left) &&
                     constant_integer_on_path(comparison->right(), current, current.size(), right);
        if (!known && affine_integer_on_path(comparison->left(), current, current.size(), affine_left) &&
            affine_integer_on_path(comparison->right(), current, current.size(), affine_right) &&
            affine_left.base == affine_right.base && affine_left.definition == affine_right.definition) {
          left = affine_left.offset; right = affine_right.offset; known = true;
        }
        if (type != nullptr && comparison->right()->type() == type &&
            known) {
          bool feasible = true;
          switch (comparison->predicate()) {
            case ar::Comparison::SIEQ: case ar::Comparison::UIEQ: feasible = left == right; break;
            case ar::Comparison::SINE: case ar::Comparison::UINE: feasible = left != right; break;
            case ar::Comparison::SIGT: if (type->is_signed()) feasible = left > right; break;
            case ar::Comparison::SIGE: if (type->is_signed()) feasible = left >= right; break;
            case ar::Comparison::SILT: if (type->is_signed()) feasible = left < right; break;
            case ar::Comparison::SILE: if (type->is_signed()) feasible = left <= right; break;
            case ar::Comparison::UIGT: if (!type->is_signed()) feasible = left > right; break;
            case ar::Comparison::UIGE: if (!type->is_signed()) feasible = left >= right; break;
            case ar::Comparison::UILT: if (!type->is_signed()) feasible = left < right; break;
            case ar::Comparison::UILE: if (!type->is_signed()) feasible = left <= right; break;
            default: break;
          }
          if (!feasible) return;
        }
      }
      // 死路径终止：ar::Unreachable，或 abort()/exit()（LibcAbort/LibcExit）——
      // 否则 abort 分支会「掉回」调用方误报竞争。
      if (core::isa< ar::Unreachable >(s)) {
        in_stack.pop_back();
        return;
      }
      if (auto* call = core::dyn_cast< ar::CallBase >(s)) {
        auto* f = exact_callee(call);
        if (f != nullptr && (f->intrinsic_id() == ar::Intrinsic::LibcAbort ||
                             f->intrinsic_id() == ar::Intrinsic::LibcExit)) return;
      }
      BmcEvent ev;
      if (classify_event(s, ev, &in_stack)) {
        // check_race currently orders the opponent's last event before every
        // join. A join before our target therefore makes adjacency UNSAT.
        // This is query pruning, not a claim about actual join handle identity.
        if (budget.prune_join_prefix && ev.kind == EvKind::Join &&
            candidate_index(current) == current.size()) {
          budget.rejected_reason = "join_orders_target";
          budget.rejected_statement = s;
          return;
        }
        current.push_back(ev);
      } else if (auto* call = core::dyn_cast< ar::CallBase >(s)) {
        // Encode the actual nonzero argument, including casts. Failure to
        // extract a comparison must not turn an assume into a no-op.
        current.emplace_back(s, is_assume_call(call) ? EvKind::Assume : EvKind::Local);
      } else if (total_local_statement(s)) {
        current.emplace_back(s, EvKind::Local);
      } else {
        BmcEvent unsupported(s, EvKind::Unsupported);
        unsupported.blocker = "prefix_statement";
        current.push_back(unsupported);
      }
      if (current.size() > MAX_PATH_EVENTS) {
        budget.exhausted = "unroll_event_budget";
        return;
      }
      if (current.back().kind == EvKind::Unsupported) {
        budget.rejected_reason = current.back().blocker;
        budget.rejected_statement = s;
        return; // This executed prefix cannot reach a supported SAT query.
      }
      if (s == target) {
        auto occurrences = std::count_if(current.begin(), current.end(),
                                          [&](const BmcEvent& event) {return !event.synthetic_atomic && event.stmt == target;});
        if (occurrences >= (target == budget.root_target ? budget.target_occurrence : 1)) {
          if (target != budget.root_target) {
            out.push_back(current);
            in_stack.pop_back();
            return;
          }
        }
      }
      if (s == budget.root_target && candidate_index(current) == current.size() &&
          (target == budget.root_target || budget.completed || open_atomic_prefix(current))) {
        auto occurrences = std::count_if(current.begin(), current.end(),
            [&](const BmcEvent& event) {return !event.synthetic_atomic && event.stmt == budget.root_target;});
        if (occurrences >= budget.target_occurrence) current.back().candidate = true;
      }
      // Continue the real CFG until the selected atomic region is closed.
      if (target == budget.root_target && candidate_index(current) < current.size() &&
          !open_atomic_prefix(current)) {
        out.push_back(current);
        in_stack.pop_back();
        return;
      }
      if (auto* call = core::dyn_cast< ar::CallBase >(s)) {
        BmcEvent classified;
        if (classify_event(s, classified, &in_stack) || is_assume_call(call) ||
            supported_leaf_call(call)) continue;
        auto* known = callee_on_path(call, current, current.size() - 1);
        auto alternatives = known != nullptr ? std::vector< ar::Function* >{known} : indirect_targets(call);
        if (alternatives.empty()) {
          budget.rejected_reason = "call_summary"; budget.rejected_statement = s; return;
        }
        for (auto* callee : alternatives) {
          auto prefix = current;
          auto frames = in_stack;
          if (known == nullptr && std::any_of(frames.begin(), frames.end(), [&](auto* block) {
                return block->code() == callee->body_or_null();
              })) {
            budget.rejected_reason = "recursive_callback"; budget.rejected_statement = s; continue;
          }
          if (depth >= MAX_INLINE_DEPTH || !inline_ok(callee)) {
            budget.rejected_reason = "call_summary";
            budget.rejected_statement = s;
            continue;
          }
          prefix.back().called_function = callee;
          const bool atomic = named_verifier_atomic(callee);
          if (atomic) {
            BmcEvent begin(s, EvKind::Lock, PSEUDO_ATOMIC_LOCK);
            begin.synthetic_atomic = true;
            prefix.push_back(begin);
          }
          // First search for a target inside the callee. Then compose every
          // concrete return prefix with the rest of the caller; never discard it.
          auto callee_path = prefix;
          dfs_paths(callee->body_or_null()->entry_block(), target, callee_path,
                    out, frames, budget, depth + 1);
          std::vector< std::vector< BmcEvent > > returns;
          for (ar::BasicBlock* block : *callee->body_or_null()) {
            for (ar::Statement* ret : *block) {
              if (!core::isa< ar::ReturnValue >(ret)) continue;
              auto return_path = prefix;
              dfs_paths(callee->body_or_null()->entry_block(), ret, return_path,
                        returns, frames, budget, depth + 1);
            }
          }
          frames.pop_back(); // resuming the same caller block is not a backedge
          for (auto& returned : returns) {
            if (atomic) {
              BmcEvent end(s, EvKind::Unlock, PSEUDO_ATOMIC_LOCK);
              end.synthetic_atomic = true;
              returned.push_back(end);
            }
            if (returned.size() > MAX_PATH_EVENTS) {
              budget.exhausted = "unroll_event_budget";
              return;
            }
            if (atomic && target == budget.root_target && candidate_index(returned) < returned.size() &&
                !open_atomic_prefix(returned)) {
              out.push_back(returned);
              continue;
            }
            dfs_paths(bb, target, returned, out, frames, budget, depth, s);
          }
        }
        return;
      }
    }
  }

  for (auto succ = bb->successor_begin(), end = bb->successor_end(); succ != end; ++succ) {
    auto path = current;
    dfs_paths(*succ, target, path, out, in_stack, budget, depth);
  }

  if (!on_stack) {
    in_stack.pop_back();
  }
}

/// 展开结果：路径集合 + 是否不完整（target 在循环体内或循环之后）。
struct UnrollResult {
  std::vector< std::vector< BmcEvent > > paths;
  bool incomplete = false;  // true = construction budget exhausted
  const char* reason = nullptr;
  const char* rejected_reason = nullptr;
  ar::Statement* rejected_statement = nullptr;
};

/// 从函数入口到 `target` 的所有路径（事件序列）。
inline UnrollResult unroll_to(
    ar::Function* func, ar::Statement* target,
    unsigned target_occurrence = 1, unsigned loop_bound = 1,
    bool prune_join_prefix = false) {
  UnrollResult result;
  // A root invocation has no source call on which to attach the boundaries.
  if (named_verifier_atomic(func)) {
    result.rejected_reason = "atomic_root";
    result.rejected_statement = target;
    return result;
  }
  ar::Code* body = func->body_or_null();
  if (body == nullptr || !body->has_entry_block()) {
    return result;
  }
  UnrollBudget budget;
  budget.prune_join_prefix = prune_join_prefix;
  budget.target_occurrence = target_occurrence;
  budget.loop_bound = loop_bound;
  budget.root_target = target;
  std::vector< BmcEvent > current;
  std::vector< ar::BasicBlock* > in_stack;
  dfs_paths(body->entry_block(), target, current, result.paths, in_stack,
            budget);
  result.rejected_reason = budget.rejected_reason;
  result.rejected_statement = budget.rejected_statement;

  // Enumeration can stop after complete prefixes were emitted. Keep those;
  // the interrupted DFS branch was never emitted and cannot become a witness.
  if (budget.exhausted != nullptr) {
    result.incomplete = true;
    result.reason = budget.exhausted;
    return result;
  }

  // Every retained prefix follows actual CFG guards. Backedges are cut,
  // so this is bounded search, never a proof of safety. No guessed loop exits.
  return result;
}

// Atomic-target queries need both completed worker paths, including assumptions
// following the opposite access. Do not replace them with arbitrary stop points.
inline UnrollResult unroll_completed(ar::Function* func, ar::Statement* target,
                                     unsigned loop_bound) {
  UnrollResult result;
  auto* body = func->body_or_null();
  if (named_verifier_atomic(func) || body == nullptr || !body->has_entry_block()) return result;
  UnrollBudget budget;
  budget.root_target = target;
  budget.completed = true;
  budget.loop_bound = loop_bound;
  for (auto* block : *body) for (auto* statement : *block) {
    if (!core::isa< ar::ReturnValue >(statement) &&
        !thread_exit_call(core::dyn_cast< ar::CallBase >(statement))) continue;
    std::vector< BmcEvent > current;
    dfs_paths(body->entry_block(), statement, current, result.paths, {}, budget);
  }
  result.rejected_reason = budget.rejected_reason;
  result.rejected_statement = budget.rejected_statement;
  if (budget.exhausted != nullptr) {
    result.incomplete = true; result.reason = budget.exhausted;
  }
  result.paths.erase(std::remove_if(result.paths.begin(), result.paths.end(),
      [](const auto& path) {return candidate_index(path) == path.size();}), result.paths.end());
  return result;
}

}  // namespace bmc
}  // namespace analyzer
}  // namespace ikos
