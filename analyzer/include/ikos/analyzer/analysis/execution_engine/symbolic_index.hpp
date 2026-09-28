/*******************************************************************************
 *
 * \file
 * \brief Symbolic array-index extraction for flat-array race detection
 *
 * Author: ikos-race-detection
 *
 * \copyright See data_race.hpp for the full license.
 *
 * These helpers recover the SYMBOLIC SLOT of a collapsed array element
 * (`&mutex[i]`, `slot[i]`) from the AR PointerShift that defines it, WITHOUT
 * any CFG list-traversal or heap `->next` pointer-chasing (the latter is what
 * made list-node region tainting unsound under cross-list linking — see
 * 09-regions_23-evilcollapse_rc.c). The slot identity is the canonical INDEX
 * VARIABLE (plus coeff/const so `j` differs from `j+1`); the array's element
 * size and base address are deliberately NOT part of the identity — the lock
 * array and data array share the slot index but differ in stride/address.
 *
 ******************************************************************************/

#pragma once

#include <ikos/ar/semantic/code.hpp>
#include <ikos/ar/semantic/statement.hpp>
#include <ikos/ar/semantic/value.hpp>

#include <ikos/analyzer/analysis/call_context.hpp>

#include <ikos/core/domain/concurrent_global_env.hpp>

#include <cstdint>

namespace ikos {
namespace analyzer {

/// \brief A symbolic linear index `coeff * var + const_term`, where `var` is
/// identified by its AR InternalVariable pointer (stable within a run — the
/// same SSA value maps to the same pointer on both the lock and data side).
struct LinearIndex {
  std::uint64_t var_id = 0;
  std::int64_t coeff = 1;
  std::int64_t const_term = 0;
};

/// \brief Unique defining statement of an AR InternalVariable, or nullptr when
/// it has none (function argument) or several (an SSA phi lowered as one copy
/// per predecessor). Several defs ⇒ treat as a canonical leaf, not a def-use
/// chain.
inline ar::Statement* unique_def(ar::InternalVariable* iv) {
  ar::Code* code = iv->code();
  if (code == nullptr) {
    return nullptr;
  }
  ar::Statement* found = nullptr;
  for (ar::BasicBlock* bb : *code) {
    for (ar::Statement* s : *bb) {
      if (s->result_or_null() == iv) {
        if (found != nullptr) {
          return nullptr; // phi: multiple defs
        }
        found = s;
      }
    }
  }
  return found;
}

/// \brief Base pointer of the PointerShift that defines `ptr` (container_of
/// inverse), or nullptr when `ptr` is not the result of a PointerShift.
inline ar::Value* pointershift_base(ar::Value* ptr) {
  if (auto* iv = dyn_cast< ar::InternalVariable >(ptr)) {
    ar::Code* code = iv->code();
    if (code == nullptr) {
      return nullptr;
    }
    for (ar::BasicBlock* bb : *code) {
      for (ar::Statement* s : *bb) {
        if (auto* ps = dyn_cast< ar::PointerShift >(s)) {
          if (ps->result() == iv) {
            return ps->pointer();
          }
        }
      }
    }
  }
  return nullptr;
}

/// \brief Canonicalize an AR index value into a linear form
/// `coeff * var + const_term` over a SINGLE canonical variable. Returns false
/// when the value is not a clean affine form. Value-preserving casts
/// (sext/zext/trunc/bitcast/ptr-int) and constant add/sub/mul are folded; a
/// leaf (phi, load, call result) becomes `1*var + 0`. Bailing is the sound
/// direction: the caller leaves the lock/data un-tagged (FP, never FN).
inline bool resolve_linear_index(ar::Value* v, LinearIndex& out) {
  auto* iv = dyn_cast< ar::InternalVariable >(v);
  if (iv == nullptr) {
    return false; // constant / global / local / function pointer
  }
  ar::Statement* def = unique_def(iv);
  if (def == nullptr) {
    // phi / argument / otherwise-unbound: canonical leaf
    out.var_id = reinterpret_cast< std::uint64_t >(iv);
    out.coeff = 1;
    out.const_term = 0;
    return true;
  }
  if (auto* as = dyn_cast< ar::Assignment >(def)) {
    return resolve_linear_index(as->operand(), out);
  }
  if (auto* un = dyn_cast< ar::UnaryOperation >(def)) {
    switch (un->op()) {
      case ar::UnaryOperation::SExt:
      case ar::UnaryOperation::ZExt:
      case ar::UnaryOperation::UTrunc:
      case ar::UnaryOperation::STrunc:
      case ar::UnaryOperation::Bitcast:
      case ar::UnaryOperation::PtrToUI:
      case ar::UnaryOperation::UIToPtr:
        return resolve_linear_index(un->operand(), out);
      default:
        out.var_id = reinterpret_cast< std::uint64_t >(iv);
        out.coeff = 1;
        out.const_term = 0;
        return true;
    }
  }
  if (auto* bin = dyn_cast< ar::BinaryOperation >(def)) {
    ar::Value* lhs = bin->left();
    ar::Value* rhs = bin->right();
    auto* lc = dyn_cast< ar::IntegerConstant >(lhs);
    auto* rc = dyn_cast< ar::IntegerConstant >(rhs);
    auto op = bin->op();
    const bool is_add = op == ar::BinaryOperation::SAdd ||
                        op == ar::BinaryOperation::UAdd;
    const bool is_sub = op == ar::BinaryOperation::SSub ||
                        op == ar::BinaryOperation::USub;
    const bool is_mul = op == ar::BinaryOperation::SMul ||
                        op == ar::BinaryOperation::UMul;
    if (is_mul && (lc != nullptr || rc != nullptr)) {
      std::int64_t k = (lc != nullptr ? lc : rc)
                           ->value().to_z_number().to< std::int64_t >();
      ar::Value* other = (lc != nullptr) ? rhs : lhs;
      LinearIndex sub;
      if (resolve_linear_index(other, sub)) {
        out.var_id = sub.var_id;
        out.coeff = sub.coeff * k;
        out.const_term = sub.const_term * k;
        return true;
      }
      return false;
    }
    if ((is_add || is_sub) && rc != nullptr) {
      std::int64_t k = rc->value().to_z_number().to< std::int64_t >();
      LinearIndex sub;
      if (resolve_linear_index(lhs, sub)) {
        out.var_id = sub.var_id;
        out.coeff = sub.coeff;
        out.const_term = sub.const_term + (is_add ? k : -k);
        return true;
      }
      return false;
    }
    if (is_sub && lc != nullptr) {
      std::int64_t k = lc->value().to_z_number().to< std::int64_t >();
      LinearIndex sub;
      if (resolve_linear_index(rhs, sub)) {
        out.var_id = sub.var_id;
        out.coeff = -sub.coeff;
        out.const_term = k - sub.const_term;
        return true;
      }
      return false;
    }
    return false; // non-affine binary
  }
  // leaf (load, pointer-shift, call result, ...): canonical identity
  out.var_id = reinterpret_cast< std::uint64_t >(iv);
  out.coeff = 1;
  out.const_term = 0;
  return true;
}

/// \brief Extract the SymbolicIndex from an array-element PointerShift: one
/// with a single variable index term. The element size is dropped (see file
/// header). Returns false on a constant-offset / multi-index shift.
inline bool symbolic_index_of_ps(
    ar::PointerShift* ps, core::ConcurrentGlobalEnv::SymbolicIndex& out) {
  std::uint64_t var_id = 0;
  std::int64_t coeff = 1;
  std::int64_t const_term = 0;
  bool found = false;
  for (std::size_t i = 0; i < ps->num_terms(); ++i) {
    auto term = ps->term(i);
    if (isa< ar::IntegerConstant >(term.second)) {
      continue; // constant field offset — not part of the slot identity
    }
    if (found) {
      return false; // >1 variable term: not a single-index access
    }
    LinearIndex li;
    if (!resolve_linear_index(term.second, li)) {
      return false;
    }
    var_id = li.var_id;
    coeff = li.coeff;
    const_term = li.const_term;
    found = true;
  }
  if (!found) {
    return false; // no variable index (constant offset)
  }
  out.var_id = var_id;
  out.coeff = coeff;
  out.const_term = const_term;
  return true;
}

/// \brief Sum of the constant ARRAY-ELEMENT indices in a PointerShift: the
/// terms whose coefficient is > 1. The frontend (translate_getelementptr) emits
/// a STRUCT-FIELD term with coefficient 1 (`1 * offsetof(field)`) and an
/// ARRAY-ELEMENT term with coefficient = element size (`size * index`), so
/// coefficient > 1 ⟹ the value is an ELEMENT index (`entry[1]` shifts by
/// 1 * sizeof), coefficient == 1 ⟹ the value is a FIELD offset (same element,
/// e.g. `entry->refs_mutex`). The element index advances the slot; the field
/// offset does not — conflating them made `entry[1].refs` recover slot {i}
/// instead of {i+1} (06-symbeq_39-funloop_index_bad.c FN). A `char`-array
/// element (size 1) is indistinguishable from a field here and is dropped — the
/// pre-existing behaviour, never a regression.
inline std::int64_t constant_element_offset(ar::PointerShift* ps) {
  std::int64_t acc = 0;
  for (std::size_t i = 0; i < ps->num_terms(); ++i) {
    auto term = ps->term(i);
    auto* c = dyn_cast< ar::IntegerConstant >(term.second);
    if (c != nullptr && term.first.to_z_number() > core::ZNumber(1)) {
      acc += c->value().to_z_number().to< std::int64_t >();
    }
  }
  return acc;
}

/// \brief Extract the SymbolicIndex of an array-element pointer value (e.g.
/// `&mutex[i]`): find its defining PointerShift and extract.
///
/// Peels through NESTED PointerShifts so a lock argument reached via an
/// intermediate local pointer still recovers its variable index: for
/// `&p->mutex` where `p = &a[i]`, the outer shift is a CONSTANT field offset
/// (`p + 8`) with the variable index buried one level down — the data-side
/// `region_collect` already traces this depth, so the lock side must too
/// (06-symbeq_06-tricky_address1.c FP). A constant field offset does not
/// change the slot identity (same as container_of), so peeling it is sound:
/// strictly MORE locks are admitted, which only ever suppresses.
///
/// Also resolves a FORMAL parameter to its actual argument at the current call
/// site (06-symbeq_02-funloop_norace.c FP: the lock `&entry->refs_mutex` lives
/// inside `cache_entry_addref`, whose `entry` is bound to `&cache[i]` only via
/// `match_down`, not syntactically). Pass the analyzer's CallContext so a
/// formal peels to `&cache[i]` and recovers `{i}`; nullptr (default) disables
/// this step.
inline bool symbolic_index_of(
    ar::Value* ptr, core::ConcurrentGlobalEnv::SymbolicIndex& out,
    CallContext* cc = nullptr) {
  std::int64_t acc = 0; // element offsets accumulated across peeled shifts
  for (int depth = 0; depth < 8; ++depth) {
    auto* iv = dyn_cast< ar::InternalVariable >(ptr);
    if (iv == nullptr) {
      return false;
    }
    ar::Statement* def = unique_def(iv);
    auto* ps = (def != nullptr) ? dyn_cast< ar::PointerShift >(def) : nullptr;
    if (ps != nullptr) {
      if (symbolic_index_of_ps(ps, out)) {
        // Variable index found: fold in the element offsets peeled through on
        // the way here — `entry[1].refs` shifted by 1 element, so its slot is
        // {i+1}, not {i} (06-symbeq_39-funloop_index_bad.c FN).
        out.const_term += acc;
        return true;
      }
      acc += constant_element_offset(ps);
      ptr = ps->pointer(); // constant field offset: peel to the base pointer
      continue;
    }
    // No PointerShift def: a formal parameter (or phi). Resolve a formal to
    // its actual argument at the current call site and keep peeling.
    bool resolved = false;
    if (cc != nullptr && !cc->empty()) {
      ar::Code* code = iv->code();
      ar::Function* fun = code ? code->function_or_null() : nullptr;
      if (fun != nullptr) {
        for (std::size_t i = 0; i < fun->num_parameters(); ++i) {
          if (fun->param(i) != iv) {
            continue;
          }
          auto* call = dyn_cast< ar::CallBase >(cc->call());
          auto* fpc = call ? dyn_cast_or_null< ar::FunctionPointerConstant >(
                                 call->called())
                           : nullptr;
          if (call != nullptr && fpc != nullptr && fpc->function() == fun &&
              i < call->num_arguments()) {
            ptr = call->argument(i);
            cc = cc->parent();
            resolved = true;
          }
          break; // iv is param i (resolved or not); stop scanning params
        }
      }
    }
    if (!resolved) {
      return false; // phi / unresolvable formal
    }
  }
  return false;
}

/// \brief Extract the SSA base-pointer identity of a `&base->field` pointer
/// whose base is a POINTER InternalVariable (a polymorphic base `s` in
/// `s = x ? &A : &B; m = &s->mutex; d = &s->datum`). Peels value-preserving
/// casts and one PointerShift; requires the shift to carry only constant
/// field terms. The identity is the base pointer itself (SSA: `m` and `d`
/// reference the SAME `s`), NOT the field offset (dropped, like the array
/// stride). A global array base (`&a[i]`) is handled by `symbolic_index_of`
/// and returns false here (its base is a GlobalVariable, not an InternalVar).
inline bool symbolic_base_of(
    ar::Value* ptr, core::ConcurrentGlobalEnv::SymbolicIndex& out) {
  ar::Value* cur = ptr;
  for (int depth = 0; depth < 8; ++depth) {
    auto* iv = dyn_cast< ar::InternalVariable >(cur);
    if (iv == nullptr) {
      return false;
    }
    ar::Statement* def = unique_def(iv);
    if (auto* ps = dyn_cast_or_null< ar::PointerShift >(def)) {
      auto* base_iv = dyn_cast< ar::InternalVariable >(ps->pointer());
      if (base_iv == nullptr) {
        return false; // global/array base: array case, handled elsewhere
      }
      // The base must be a polymorphic PHI (`s = x ? &A : &B`), not:
      //   - a Load / PointerShift / Call result (carries an array index or a
      //     deref that region_collect must trace — arraylist_nr's `slot[i]`
      //     is `(load @slot+8*i)`),
      //   - a FORMAL parameter (bound via the call site in match_down, not a
      //     syntactic def; region_collect resolves it to the actual argument
      //     — arraylist_nr's inlined `list` maps to `slot[i]` -> {i}).
      // The base must be a polymorphic PHI of GLOBAL addresses
      // (`s = x ? &A : &B`): every def is a copy (value-preserving casts ok)
      // of a GlobalVariable. A LOOP variable (`pos` updated by `container_of`
      // each iteration) instead has defs that are copies of PointerShift
      // chains — treating it as a phi would push `{pos}` as a region and
      // collide with the initial def's `{j}` slot index, so `self_locked`
      // never matches (09-regions_21-arrayloop2_nr.c FP). Reject any base
      // whose defs do not all peel (through casts) to a GlobalVariable.
      {
        ar::Code* base_code = base_iv->code();
        if (base_code == nullptr) {
          return false;
        }
        int ndefs = 0;
        for (ar::BasicBlock* bb : *base_code) {
          for (ar::Statement* st : *bb) {
            if (st->result_or_null() != base_iv) {
              continue;
            }
            ++ndefs;
            ar::Value* v = nullptr;
            if (auto* as = dyn_cast< ar::Assignment >(st)) {
              v = as->operand();
            } else if (auto* un = dyn_cast< ar::UnaryOperation >(st)) {
              switch (un->op()) {
                case ar::UnaryOperation::Bitcast:
                case ar::UnaryOperation::PtrToUI:
                case ar::UnaryOperation::UIToPtr:
                  v = un->operand();
                  break;
                default:
                  return false;
              }
            } else {
              return false; // PointerShift/Load/Call def: not a global-addr phi
            }
            bool is_global = false;
            for (int d = 0; d < 8; ++d) {
              if (isa< ar::GlobalVariable >(v)) {
                is_global = true;
                break;
              }
              auto* viv = dyn_cast< ar::InternalVariable >(v);
              if (viv == nullptr) {
                break;
              }
              ar::Statement* vd = unique_def(viv);
              if (auto* un2 = dyn_cast_or_null< ar::UnaryOperation >(vd)) {
                switch (un2->op()) {
                  case ar::UnaryOperation::Bitcast:
                  case ar::UnaryOperation::PtrToUI:
                  case ar::UnaryOperation::UIToPtr:
                    v = un2->operand();
                    continue;
                  default:
                    break;
                }
              }
              break;
            }
            if (!is_global) {
              return false;
            }
          }
        }
        if (ndefs < 2) {
          return false; // single/zero def: not a phi (formal handled below)
        }
      }
      if (ar::Code* base_code = base_iv->code()) {
        if (ar::Function* fun = base_code->function_or_null()) {
          for (std::size_t i = 0; i < fun->num_parameters(); ++i) {
            if (fun->param(i) == base_iv) {
              return false; // formal parameter
            }
          }
        }
      }
      for (std::size_t i = 0; i < ps->num_terms(); ++i) {
        if (!isa< ar::IntegerConstant >(ps->term(i).second)) {
          return false; // a variable index term: not a pure field offset
        }
      }
      out.var_id = reinterpret_cast< std::uint64_t >(base_iv);
      out.coeff = 1;
      out.const_term = 0;
      return true;
    }
    if (auto* un = dyn_cast_or_null< ar::UnaryOperation >(def)) {
      switch (un->op()) {
        case ar::UnaryOperation::Bitcast:
        case ar::UnaryOperation::PtrToUI:
        case ar::UnaryOperation::UIToPtr:
          cur = un->operand();
          break;
        default:
          return false;
      }
      continue;
    }
    return false; // phi / call / other non-pointer-shift leaf
  }
  return false;
}

} // end namespace analyzer
} // end namespace ikos
