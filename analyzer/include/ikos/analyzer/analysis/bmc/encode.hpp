// Candidate-prefix encoding: clocks, selected guards/addresses, spawn/join,
// mutex intervals and complete verifier atomic blocks. Confirmation also needs
// supported value provenance, object bounds and concrete thread activation.
// Missing read-from/value dependencies are rejected, not free SMT inputs.

#pragma once

#include <z3++.h>
#include <llvm/IR/Instructions.h>

#include <functional>
#include <map>
#include <set>
#include <vector>

#include "unroll.hpp"
#include "facts.hpp"
#include "initial.hpp"
#include "replay.hpp"

namespace ikos {
namespace analyzer {
namespace bmc {

/// 判断两个访问（各线程路径的最后一个事件）是否真竞争。
/// 返回 true = 确认；false = 未确认（含 UNSAT / unsupported），不是无竞争证明。
inline bool check_race(const std::vector< BmcEvent >& inputA,
                       const std::vector< BmcEvent >& inputB,
                       ar::Function* funcA, ar::Function* funcB,
                       Diagnostics* diagnostics = nullptr,
                       const CandidateFacts* facts = nullptr) {
  auto pathA = inputA, pathB = inputB;
  if (pathA.empty() || pathB.empty()) {
    record(diagnostics, "empty_path");
    return false;
  }
  const std::vector< BmcEvent > empty_startup;
  const auto& startup = facts != nullptr ? facts->startup : empty_startup;
  auto* argumentA = created_argument(funcA, funcB->name() == "main" ? pathB : startup);
  auto* argumentB = created_argument(funcB, funcA->name() == "main" ? pathA : startup, funcA == funcB ? 1 : 0);
  std::map< std::pair< std::uint64_t, core::ZNumber >, std::uint64_t > lock_ids;
  auto prepare = [&](std::vector< BmcEvent >& path, ar::Function* root, ar::Value* argument) {
    std::set< std::uint64_t > held;
    std::uint64_t symbolic_lock = 0;
    for (std::size_t i = 0; i < path.size(); ++i) {
      auto& event = path[i];
      ar::Value* pointer = nullptr;
      if (auto* load = core::dyn_cast< ar::Load >(event.stmt)) pointer = load->operand();
      if (auto* store = core::dyn_cast< ar::Store >(event.stmt)) pointer = store->pointer();
      if (pointer != nullptr) {
        event.base = 0; event.local_base = nullptr; event.const_off = 0; event.var_id = 0; event.coeff = 0;
        if (!pointer_on_path(pointer, path, i, root, argument, event)) {record(diagnostics, "prefix_address", event.stmt); return false;}
      }
      if ((event.kind == EvKind::Lock || event.kind == EvKind::Unlock) &&
          !is_verifier_atomic(core::cast< ar::CallBase >(event.stmt))) {
        auto* call = core::cast< ar::CallBase >(event.stmt);
        BmcEvent address;
        core::ZNumber size;
        if (call->num_arguments() != 1 ||
            !pointer_on_path(call->argument(0), path, i, root, argument, address) ||
            address.base == 0 || !memory_extent(address, size)) {
          record(diagnostics, "mutex_identity", event.stmt); return false;
        }
        event.base = address.base; event.const_off = address.const_off;
        event.var_id = address.var_id; event.var_before = address.var_before; event.coeff = address.coeff;
        auto* type = core::dyn_cast< ar::PointerType >(call->argument(0)->type());
        if (type == nullptr || type->pointee()->is_opaque()) return false;
        event.width = call->bundle()->data_layout().store_size_in_bytes(type->pointee()).to< std::uint64_t >();
        if (event.var_id == 0 && (event.const_off < 0 || event.const_off + event.width > size)) return false;
        auto id = lock_ids.emplace(std::make_pair(address.base, address.const_off), lock_ids.size() + 1);
        if (event.kind == EvKind::Lock) {
          if (symbolic_lock != 0 || (event.var_id != 0 && !held.empty())) {
            record(diagnostics, "symbolic_mutex_nested", event.stmt); return false;
          }
          event.mutex = event.var_id == 0 ? id.first->second : (~std::uint64_t(0) - i);
          if (event.var_id != 0) symbolic_lock = event.mutex;
          held.insert(event.mutex);
        } else {
          if (symbolic_lock != 0) event.mutex = symbolic_lock;
          else if (event.var_id != 0 && held.size() == 1) event.mutex = *held.begin();
          else if (event.var_id != 0) {record(diagnostics, "mutex_ownership", event.stmt); return false;}
          else event.mutex = id.first->second;
          held.erase(event.mutex);
          symbolic_lock = 0;
        }
      }
    }
    return true;
  };
  if (!prepare(pathA, funcA, argumentA) || !prepare(pathB, funcB, argumentB) ||
      !prefix_values_defined(startup, diagnostics) ||
      !prefix_values_defined(pathA, diagnostics, funcA, argumentA) ||
      !prefix_values_defined(pathB, diagnostics, funcB, argumentB)) return false;

  z3::context ctx;
  z3::solver s(ctx);

  const std::size_t nA = pathA.size();
  const std::size_t nB = pathB.size();

  // clock 变量：A 的 nA 个 + B 的 nB 个。
  std::vector< z3::expr > clock;
  for (std::size_t i = 0; i < nA; ++i) {
    clock.push_back(ctx.int_const(("a" + std::to_string(i)).c_str()));
  }
  for (std::size_t i = 0; i < nB; ++i) {
    clock.push_back(ctx.int_const(("b" + std::to_string(i)).c_str()));
  }

  // po：路径内相邻事件 clock 递增。
  for (std::size_t i = 0; i + 1 < nA; ++i) {
    s.add(clock[i] < clock[i + 1]);
  }
  for (std::size_t i = 0; i + 1 < nB; ++i) {
    s.add(clock[nA + i] < clock[nA + i + 1]);
  }
  // SC memory operations have a common total order. Tied writes cannot supply
  // different "latest" values to separate reads.
  z3::expr_vector memory_clocks(ctx);
  for (std::size_t i = 0; i < nA; ++i)
    if (pathA[i].kind == EvKind::Read || pathA[i].kind == EvKind::Write) memory_clocks.push_back(clock[i]);
  for (std::size_t i = 0; i < nB; ++i)
    if (pathB[i].kind == EvKind::Read || pathB[i].kind == EvKind::Write) memory_clocks.push_back(clock[nA + i]);
  if (memory_clocks.size() > 1) s.add(z3::distinct(memory_clocks));

  // spawn HB（svcomp.cat 的 po 跨线程边）：路径里 create(T)（T = 对方线程入口函数）
  // < T 的首事件。funcA/funcB 是两线程入口函数，Create 事件的 thread 字段存线程
  // 函数指针。detach 无 join、只有 spawn 边；join 边（E_T < join）后续再加。
  std::uint64_t ta = reinterpret_cast< std::uint64_t >(funcA);
  std::uint64_t tb = reinterpret_cast< std::uint64_t >(funcB);
  for (std::size_t i = 0; i < nA; ++i) {
    if (pathA[i].kind == EvKind::Create && pathA[i].thread == tb) {
      s.add(clock[i] < clock[nA]);  // A 的 create(B) < B 首事件（clock[nA] = b_0）
    }
  }
  for (std::size_t i = 0; i < nB; ++i) {
    if (pathB[i].kind == EvKind::Create && pathB[i].thread == ta) {
      s.add(clock[nA + i] < clock[0]);  // B 的 create(A) < A 首事件（clock[0] = a_0）
    }
  }

  // join HB（svcomp.cat 的 po 跨线程边）：joined 线程末事件 < join。过近似——把
  // A 路径里每个 Join 都约束成「B 末事件 < join」（假设 B 被 A join），B 路径同理。
  // 过约束只丢 TP（unjoined 实例，2 线程抽象表达不了），不引 FP（sound）。
  for (std::size_t i = 0; i < nA; ++i) {
    if (pathA[i].kind == EvKind::Join) {
      s.add(clock[nA + nB - 1] < clock[i]);  // B 末事件 < A 的 join
    }
  }
  for (std::size_t i = 0; i < nB; ++i) {
    if (pathB[i].kind == EvKind::Join) {
      s.add(clock[nA - 1] < clock[nA + i]);  // A 末事件 < B 的 join
    }
  }

  // Keep every critical section. Last-lock maps discard earlier exclusion and
  // permit infeasible prefixes. Unsupported ownership/recursion stays UNKNOWN.
  struct Section {std::uint64_t mutex; std::size_t begin; std::size_t end; bool reader;};
  auto sections = [&](const std::vector< BmcEvent >& path, std::vector< Section >& out) {
    std::map< std::uint64_t, std::size_t > active;
    for (std::size_t i = 0; i < path.size(); ++i) {
      const auto& e = path[i];
      if (e.kind == EvKind::Unsupported) {
        record(diagnostics, e.blocker, e.stmt);
        return false;
      }
      if (e.kind != EvKind::Lock && e.kind != EvKind::Unlock) continue;
      if (e.mutex == 0) {record(diagnostics, "mutex_identity", e.stmt); return false;}
      if (e.kind == EvKind::Lock) {
        if (active.count(e.mutex)) {record(diagnostics, "mutex_relock", e.stmt); return false;}
        active[e.mutex] = i;
      } else {
        auto held = active.find(e.mutex);
        if (held == active.end()) {record(diagnostics, "mutex_ownership", e.stmt); return false;}
        out.push_back({e.mutex, held->second, i, path[held->second].reader});
        active.erase(held);
      }
    }
    if (active.count(PSEUDO_ATOMIC_LOCK)) {
      record(diagnostics, "atomic_prefix_open", path.back().stmt);
      return false;
    }
    for (const auto& entry : active) out.push_back({entry.first, entry.second, path.size(), path[entry.second].reader});
    return true;
  };
  std::vector< Section > sections_a, sections_b;
  if (!sections(pathA, sections_a) || !sections(pathB, sections_b)) return false;
  // Verifier atomic blocks exclude all other threads, including accesses
  // outside atomic blocks. A mutex-only exclusion does not express this.
  for (const auto& section : sections_a) {
    if (section.mutex == PSEUDO_ATOMIC_LOCK)
      for (std::size_t j = 0; j < nB; ++j)
        s.add(clock[nA + j] < clock[section.begin] || clock[section.end] < clock[nA + j]);
  }
  for (const auto& section : sections_b) {
    if (section.mutex == PSEUDO_ATOMIC_LOCK)
      for (std::size_t i = 0; i < nA; ++i)
        s.add(clock[i] < clock[nA + section.begin] || clock[nA + section.end] < clock[i]);
  }

  // Only source nondet is free. Other demanded values need an executed dynamic
  // definition, exact machine arithmetic, actual arguments/returns or SC rf.
  // Unsupported and uninitialized origins never become fresh SMT inputs.
  bool unencodable = false;

  // 每路径独立的 cache 与符号前缀：同一线程入口函数的两个实例，其局部变量共享同一
  // InternalVariable 指针，但不同实例互不相关——共用同一 SMT 符号会错误相关两实例
  // 的独立值（FN）。
  using ValueKey = std::pair< ar::InternalVariable*, std::size_t >;
  using ValueCache = std::map< ValueKey, z3::expr >;
  ValueCache cache_a, cache_b, cache_init;
  std::map< const BmcEvent*, z3::expr > replay_offsets;
  std::function<
      bool(ar::Value*, z3::expr&, ValueCache&, const std::string&, unsigned, std::size_t) >
      encode_value;
  encode_value = [&](ar::Value* v,
                     z3::expr& out,
                     ValueCache& cache,
                     const std::string& prefix,
                     unsigned depth, std::size_t before) -> bool {
    if (depth >= 64) {
      record(diagnostics, "value_depth");
      return false;
    }
    if (auto* c = core::dyn_cast< ar::IntegerConstant >(v)) {
      out = ctx.int_val(c->value().to_z_number().str().c_str());
      return true;
    }
    auto* iv = core::dyn_cast< ar::InternalVariable >(v);
    if (iv == nullptr) {
      record(diagnostics, "value_non_integer");
      return false;
    }
    std::size_t definition_index = 0;
    const auto& path = prefix == "value_a_" ? pathA : prefix == "value_b_" ? pathB : startup;
    ar::Statement* def = reaching_definition(iv, path, before, definition_index);
    std::size_t call_index = 0;
    auto* actual = def == nullptr ? formal_argument(iv, path, before, call_index) : nullptr;
    if (actual != nullptr) {
      if (actual->type() != iv->type() || !encode_value(actual, out, cache, prefix, depth + 1, call_index)) return false;
      return true;
    }
    if (def == nullptr) {
      record(diagnostics, "value_no_definition");
      return false;
    }
    ValueKey key{iv, definition_index};
    auto cached = cache.find(key);
    if (cached != cache.end()) {out = cached->second; return true;}
    if (auto* as = core::dyn_cast_or_null< ar::Assignment >(def)) {
      if (as->operand()->type() != iv->type() ||
          !encode_value(as->operand(), out, cache, prefix, depth + 1, definition_index)) {
        return false;
      }
      cache.emplace(key, out);
      return true;
    }
    if (auto* un = core::dyn_cast_or_null< ar::UnaryOperation >(def)) {
      auto* src = core::dyn_cast< ar::IntegerType >(un->operand()->type());
      auto* dst = core::dyn_cast< ar::IntegerType >(iv->type());
      if (src == nullptr || dst == nullptr) {
        record(diagnostics, "value_non_integer_cast", def);
        return false;
      }
      bool extension = dst->bit_width() > src->bit_width() &&
                       (un->op() == ar::UnaryOperation::SExt || un->op() == ar::UnaryOperation::ZExt);
      bool bitcast = un->op() == ar::UnaryOperation::Bitcast &&
                     src->bit_width() == dst->bit_width();
      bool truncation = dst->bit_width() < src->bit_width() &&
                        (un->op() == ar::UnaryOperation::UTrunc || un->op() == ar::UnaryOperation::STrunc);
      if (!extension && !bitcast && !truncation) {
        record(diagnostics, "value_cast", def);
        return false;
      }
      if (!encode_value(un->operand(), out, cache, prefix, depth + 1, definition_index)) {
        return false;
      }
      if ((bitcast && src->is_signed() == dst->is_signed()) ||
          (extension && un->op() == ar::UnaryOperation::SExt && src->is_signed() && dst->is_signed()) ||
          (extension && un->op() == ar::UnaryOperation::ZExt && !src->is_signed())) {
        cache.emplace(key, out);
        return true;
      }
      z3::expr bits = z3::int2bv(src->bit_width(), out);
      if (extension) bits = un->op() == ar::UnaryOperation::SExt
                                ? z3::sext(bits, dst->bit_width() - src->bit_width())
                                : z3::zext(bits, dst->bit_width() - src->bit_width());
      if (truncation) bits = bits.extract(dst->bit_width() - 1, 0);
      out = z3::bv2int(bits, dst->is_signed());
      cache.emplace(key, out);
      return true;
    }
    if (auto* binary = core::dyn_cast_or_null< ar::BinaryOperation >(def)) {
      auto* type = core::dyn_cast< ar::IntegerType >(iv->type());
      if (type == nullptr || binary->left()->type() != iv->type() || binary->right()->type() != iv->type()) return false;
      z3::expr left(ctx), right(ctx);
      if (!encode_value(binary->left(), left, cache, prefix, depth + 1, definition_index) ||
          !encode_value(binary->right(), right, cache, prefix, depth + 1, definition_index)) return false;
      unsigned width = type->bit_width();
      z3::expr l = z3::int2bv(width, left), r = z3::int2bv(width, right), result(ctx);
      z3::expr lo = ctx.int_val((type->is_signed() ? -(ZNumber(1) << (width - 1)) : ZNumber(0)).str().c_str());
      z3::expr hi = ctx.int_val(((ZNumber(1) << (type->is_signed() ? width - 1 : width)) - 1).str().c_str());
      z3::expr arithmetic(ctx);
      switch (binary->op()) {
        case ar::BinaryOperation::SAdd: case ar::BinaryOperation::UAdd: arithmetic = left + right; result = l + r; break;
        case ar::BinaryOperation::SSub: case ar::BinaryOperation::USub: arithmetic = left - right; result = l - r; break;
        case ar::BinaryOperation::SMul: case ar::BinaryOperation::UMul: arithmetic = left * right; result = l * r; break;
        case ar::BinaryOperation::SAnd: case ar::BinaryOperation::UAnd: result = l & r; break;
        case ar::BinaryOperation::SOr: case ar::BinaryOperation::UOr: result = l | r; break;
        case ar::BinaryOperation::SXor: case ar::BinaryOperation::UXor: result = l ^ r; break;
        case ar::BinaryOperation::SDiv: case ar::BinaryOperation::UDiv:
        case ar::BinaryOperation::SRem: case ar::BinaryOperation::URem: {
          s.add(right != 0);
          bool signed_op = binary->op() == ar::BinaryOperation::SDiv || binary->op() == ar::BinaryOperation::SRem;
          if (signed_op) s.add(!(left == lo && right == -1));
          if (binary->op() == ar::BinaryOperation::SDiv) result = l / r;
          else if (binary->op() == ar::BinaryOperation::UDiv) result = z3::udiv(l, r);
          else if (binary->op() == ar::BinaryOperation::SRem) result = z3::srem(l, r);
          else result = z3::urem(l, r);
          if (binary->is_exact()) s.add(result * r == l);
          break;
        }
        case ar::BinaryOperation::SShl: case ar::BinaryOperation::UShl:
        case ar::BinaryOperation::SLShr: case ar::BinaryOperation::ULShr:
        case ar::BinaryOperation::SAShr: case ar::BinaryOperation::UAShr: {
          s.add(right >= 0 && right < ctx.int_val(width));
          if (binary->op() == ar::BinaryOperation::SShl || binary->op() == ar::BinaryOperation::UShl) {
            result = z3::shl(l, r);
            if (binary->op() == ar::BinaryOperation::SShl) {
              // C signed left shift needs a nonnegative, representable source
              // execution even when LLVM did not attach a no-wrap flag.
              z3::expr wide = z3::shl(z3::zext(l, width), z3::zext(r, width));
              s.add(left >= 0 && z3::bv2int(wide, false) <= hi);
            }
            if (binary->has_no_wrap()) {
              z3::expr wide = z3::shl(type->is_signed() ? z3::sext(l, width) : z3::zext(l, width), z3::zext(r, width));
              s.add(z3::bv2int(wide, type->is_signed()) == z3::bv2int(result, type->is_signed()));
            }
          } else {
            result = binary->op() == ar::BinaryOperation::SAShr || binary->op() == ar::BinaryOperation::UAShr
                         ? z3::ashr(l, r) : z3::lshr(l, r);
            if (binary->is_exact()) s.add(z3::shl(result, r) == l);
          }
          break;
        }
        default: record(diagnostics, "value_arithmetic", def); return false;
      }
      if (static_cast< bool >(arithmetic) && binary->has_no_wrap()) {
        s.add(arithmetic >= lo && arithmetic <= hi);
        // Within the AR no-wrap range, mathematical and machine results are
        // identical. Avoid an int/BV round trip in every SC counter update.
        out = arithmetic;
      } else out = z3::bv2int(result, type->is_signed());
      cache.emplace(key, out);
      record(diagnostics, "integer_operation", def);
      return true;
    }
    if (auto* load = core::dyn_cast_or_null< ar::Load >(def)) {
      auto* type = core::dyn_cast< ar::IntegerType >(iv->type());
      const auto& access = path[definition_index];
      if (type == nullptr || abstract_initializer(def)) return false;
      // Cache the read before following writes: legal dependencies may be
      // syntactically cyclic, but strict SC source order rules out rf cycles.
      out = ctx.int_const((prefix + "read_" + std::to_string(definition_index)).c_str());
      cache.emplace(key, out);
      auto convert = [&](const z3::expr& v) {return z3::bv2int(z3::int2bv(type->bit_width(), v), type->is_signed()).simplify();};
      auto address_offset = [&](const BmcEvent& event, z3::expr& offset,
                                ValueCache& values, const std::string& label) {
        offset = ctx.int_val(event.const_off.str().c_str());
        if (event.var_id == 0) return true;
        z3::expr index(ctx);
        if (!encode_value(reinterpret_cast< ar::InternalVariable* >(event.var_id), index,
                          values, label, depth + 1, event.var_before)) return false;
        offset = offset + ctx.int_val(event.coeff.str().c_str()) * index;
        return true;
      };
      z3::expr read_offset(ctx);
      if (!address_offset(access, read_offset, cache, prefix)) return false;
      z3::expr initial(ctx);
      bool known = false;
      if (access.base != 0) {
        ZNumber bits;
        auto* global = reinterpret_cast< ar::GlobalVariable* >(access.base);
        if (access.var_id == 0) {
          known = initial_integer(global, access.const_off, type->bit_width(), bits);
          if (known) initial = convert(ctx.int_val(bits.str().c_str()));
        } else {
          // Enumerate actual typed initializer fields, including a[i].field.
          // Padding/undef and large objects remain unsupported initial sources.
          ZNumber size, stride = access.coeff < 0 ? -access.coeff : access.coeff;
          if (stride != 0 && memory_extent(access, size) && size / stride <= 256) {
            ZNumber offset = access.const_off % stride;
            if (offset < 0) offset += stride;
            bool complete = true, first = true;
            for (; offset + access.width <= size; offset += stride) {
              if (!initial_integer(global, offset, type->bit_width(), bits)) {complete = false; break;}
              auto value = convert(ctx.int_val(bits.str().c_str()));
              initial = first ? value : z3::ite(read_offset == ctx.int_val(offset.str().c_str()), value, initial).simplify();
              first = false;
            }
            known = complete && !first;
          }
        }
        // The retained startup executes before either selected child. Fold
        // actual main writes once, sharing its nondet symbols across reads.
        {
          const auto end = prefix == "value_init_" ? definition_index : startup.size();
          for (std::size_t j = 0; j < end; ++j) {
            const auto& e = startup[j];
            if (e.kind != EvKind::Write || e.base != access.base) continue;
            if (e.var_id == 0 && access.var_id == 0 &&
                (e.const_off >= access.const_off + access.width || access.const_off >= e.const_off + e.width)) continue;
            if (e.width != access.width) return false;
            z3::expr write_offset(ctx);
            if (!address_offset(e, write_offset, cache_init, "value_init_")) return false;
            s.add(write_offset == read_offset || write_offset + ctx.int_val(e.width) <= read_offset ||
                  read_offset + ctx.int_val(access.width) <= write_offset);
            auto* value = core::cast< ar::Store >(e.stmt)->value();
            auto* stored_type = core::dyn_cast< ar::IntegerType >(value->type());
            z3::expr stored(ctx);
            if (stored_type == nullptr || stored_type->bit_width() != type->bit_width() ||
                !encode_value(value, stored, cache_init, "value_init_", depth + 1, j)) return false;
            if (stored_type->is_signed() != type->is_signed()) stored = convert(stored);
            if (e.var_id == 0 && access.var_id == 0 && e.const_off == access.const_off) {
              initial = stored; known = true;
            } else {
              if (!known) return false;
              initial = z3::ite(write_offset == read_offset, stored, initial);
            }
          }
        }
      }
      std::vector< z3::expr > sources;
      struct Write {z3::expr time; z3::expr value; z3::expr matches;};
      std::vector< Write > writes;
      auto collect = [&](const std::vector< BmcEvent >& events, ValueCache& values,
                         const std::string& label, std::size_t clock_start) {
        std::size_t begin = 0;
        if (access.local_base != nullptr) {
          if (&events != &path) return true;
          for (std::size_t j = 0; j < definition_index; ++j)
            if (events[j].stmt->result_or_null() == access.local_base) begin = j + 1;
        }
        for (std::size_t j = begin; j < events.size(); ++j) {
          const auto& e = events[j];
          if (e.kind != EvKind::Write || abstract_initializer(e.stmt) || (&events == &path && j >= definition_index)) continue;
          if (access.base != 0 ? e.base != access.base : e.local_base != access.local_base) continue;
          if (e.var_id == 0 && access.var_id == 0 &&
              (e.const_off >= access.const_off + access.width || access.const_off >= e.const_off + e.width)) continue;
          if (e.width != access.width) return false;
          z3::expr write_offset(ctx);
          if (!address_offset(e, write_offset, values, label)) return false;
          s.add(write_offset == read_offset || write_offset + ctx.int_val(e.width) <= read_offset ||
                read_offset + ctx.int_val(access.width) <= write_offset);
          auto* value = core::cast< ar::Store >(e.stmt)->value();
          auto* stored_type = core::dyn_cast< ar::IntegerType >(value->type());
          z3::expr encoded(ctx);
          if (stored_type == nullptr || stored_type->bit_width() != type->bit_width() ||
              !encode_value(value, encoded, values, label, depth + 1, j)) return false;
          writes.push_back({clock[clock_start + j], stored_type->is_signed() == type->is_signed() ? encoded : convert(encoded),
                            write_offset == read_offset});
        }
        return true;
      };
      if (prefix == "value_init_") {
        // All selected children may remain unscheduled during this main
        // prefix. A read sees AR initialization or its last preceding write,
        // never a later startup store or an arbitrary fresh value.
        if (!known) {record(diagnostics, "startup_read_origin", load); return false;}
        s.add(out == initial);
        record(diagnostics, "startup_read_from", load);
        return true;
      }
      if (!collect(pathA, cache_a, "value_a_", 0) || !collect(pathB, cache_b, "value_b_", nA)) return false;
      z3::expr read_time = clock[(prefix == "value_a_" ? 0 : nA) + definition_index];
      for (std::size_t j = 0; j < writes.size(); ++j) {
        z3::expr latest = writes[j].matches && writes[j].time < read_time;
        for (std::size_t k = 0; k < writes.size(); ++k)
          if (k != j) latest = latest && !(writes[k].matches && writes[j].time < writes[k].time && writes[k].time < read_time);
        sources.push_back(latest && out == writes[j].value);
      }
      if (known) {
        z3::expr first = ctx.bool_val(true);
        for (const auto& write : writes) first = first && (!write.matches || read_time < write.time);
        sources.push_back(first && out == initial);
      }
      if (sources.empty()) return false;
      z3::expr choice = ctx.bool_val(false);
      for (const auto& source : sources) choice = choice || source;
      s.add(choice);
      record(diagnostics, "sc_read_from", load);
      return true;
    }
    if (auto* call = core::dyn_cast_or_null< ar::CallBase >(def)) {
      if (successful_create_result(path[definition_index])) {
        out = ctx.int_val(0);
        cache.emplace(key, out);
        return true;
      }
      // 识别 nondet 调用：直调是
      // FunctionPointerConstant；`__VERIFIER_nondet_int` 常被 frontend 变成 `%5
      // = bitcast @__VERIFIER_nondet_int; %6 = call %5()`， 剥 bitcast
      // 拿底层函数（sound：bitcast 不改函数身份）。
      ar::Value* called = call->called();
      bool is_nondet = false;
      bool is_bool = false;
      for (int depth = 0; depth < 4 && !is_nondet; ++depth) {
        if (auto* fpc = core::dyn_cast< ar::FunctionPointerConstant >(called)) {
          ar::Function* f = fpc->function();
          if (f != nullptr) {
            static const std::set< std::string >
                integer_sources{"__VERIFIER_nondet_bool",
                                "__VERIFIER_nondet_char",
                                "__VERIFIER_nondet_uchar",
                                "__VERIFIER_nondet_short",
                                "__VERIFIER_nondet_ushort",
                                "__VERIFIER_nondet_int",
                                "__VERIFIER_nondet_uint",
                                "__VERIFIER_nondet_long",
                                "__VERIFIER_nondet_ulong",
                                "__VERIFIER_nondet_longlong",
                                "__VERIFIER_nondet_ulonglong",
                                "__VERIFIER_nondet_size_t"};
            auto* returned =
                core::dyn_cast< ar::IntegerType >(f->type()->return_type());
            auto* result = core::dyn_cast< ar::IntegerType >(iv->type());
            // C's old-style f() declarations may be variadic in AR; check the
            // call.
            is_nondet = f->num_parameters() == 0 &&
                        call->num_arguments() == 0 && returned != nullptr &&
                        result != nullptr &&
                        returned->bit_width() == result->bit_width() &&
                        returned->is_signed() == result->is_signed() &&
                        (f->intrinsic_id() == ar::Intrinsic::IkosNonDet ||
                         (f->is_declaration() &&
                          integer_sources.count(f->name()) != 0));
            is_bool = f->name() == "__VERIFIER_nondet_bool";
          }
          break;
        }
        auto* civ = core::dyn_cast< ar::InternalVariable >(called);
        if (civ == nullptr) {
          break;
        }
        ar::Statement* cdef = unique_def_bmc(civ);
        auto* un = core::dyn_cast_or_null< ar::UnaryOperation >(cdef);
        if (un == nullptr || un->op() != ar::UnaryOperation::Bitcast) {
          break;
        }
        called = un->operand();
      }
      if (is_nondet) {
        // Abstract initializers are not executed source nondet operations.
        // Missing DebugLoc also rejects legacy unmarked freeze_uninit calls.
        auto* source = call->has_frontend()
                           ? llvm::dyn_cast< llvm::CallBase >(call->frontend< llvm::Value >())
                           : nullptr;
        if (source == nullptr || !source->getDebugLoc() ||
            source->getMetadata("ikos.freeze_uninit") != nullptr) {
          record(diagnostics, "value_nondet_origin", call);
          return false;
        }
        auto* it = core::dyn_cast< ar::IntegerType >(iv->type());
        if (it == nullptr) {
          return false;
        }
        z3::expr e =
            ctx.int_const((prefix + std::to_string(cache.size())).c_str());
        // 类型范围约束：nondet 只能取该整型范围内的值（sound，防 SMT 取到
        // 超出机器整型范围的值）。
        {
          std::uint64_t bw = it->bit_width();
          ZNumber lo, hi;
          if (it->is_signed()) {
            lo = -(ZNumber(1) << (bw - 1));
            hi = (ZNumber(1) << (bw - 1)) - 1;
          } else {
            lo = 0;
            hi = (ZNumber(1) << bw) - 1;
          }
          s.add(e >= ctx.int_val(lo.str().c_str()) &&
                e <= ctx.int_val(hi.str().c_str()));
          if (is_bool) {
            s.add(e >= 0 && e <= 1);
          }
        }
        cache.emplace(key, e);
        out = e;
        return true;
      }
      auto* callee = callee_on_path(call, path, definition_index);
      if (callee != nullptr && callee->is_definition()) {
        for (std::size_t j = definition_index + 1; j < before; ++j) {
          auto* ret = core::dyn_cast< ar::ReturnValue >(path[j].stmt);
          if (ret != nullptr && ret->code() == callee->body_or_null() &&
              invocation_call(path, ret->code(), j) == definition_index) {
            if (!ret->has_operand() || !encode_value(ret->operand(), out, cache, prefix, depth + 1, j)) return false;
            auto* type = core::dyn_cast< ar::IntegerType >(iv->type());
            auto* returned = core::dyn_cast< ar::IntegerType >(ret->operand()->type());
            if (type == nullptr || returned == nullptr || type->bit_width() != returned->bit_width()) return false;
            if (type->is_signed() != returned->is_signed())
              out = z3::bv2int(z3::int2bv(type->bit_width(), out), type->is_signed());
            cache.emplace(key, out);
            return true;
          }
        }
      }
    }
    const char* reason = "value_unsupported";
    if (def == nullptr) {
      reason = "value_no_unique_definition";
    } else if (core::isa< ar::Load >(def)) {
      reason = "value_load";
    } else if (core::isa< ar::BinaryOperation >(def)) {
      reason = "value_arithmetic";
    } else if (core::isa< ar::CallBase >(def)) {
      reason = "value_unknown_call";
    }
    record(diagnostics, reason, def);
    return false;
  };

  // Trap/poison obligations are part of execution, including unused results.
  auto arithmetic_prefix = [&](const std::vector< BmcEvent >& path, ValueCache& cache,
                                const std::string& prefix) {
    for (std::size_t i = 0; i < path.size(); ++i) {
      if (auto* op = core::dyn_cast< ar::BinaryOperation >(path[i].stmt)) {
        if (facts != nullptr && facts->no_wrap != nullptr) {
          auto proof = facts->no_wrap->find(op);
          if (proof != facts->no_wrap->end() && proof->second) {
            record(diagnostics, "ai_no_wrap", op);
            continue; // Demanded results still go through exact encode_value.
          }
        }
        z3::expr value(ctx);
        if (!encode_value(op->result(), value, cache, prefix, 0, i + 1)) {
          record(diagnostics, "prefix_arithmetic", op);
          return false;
        }
      }
    }
    return true;
  };
  if (!arithmetic_prefix(startup, cache_init, "value_init_") ||
      !arithmetic_prefix(pathA, cache_a, "value_a_") ||
      !arithmetic_prefix(pathB, cache_b, "value_b_")) return false;

  auto encode_assume = [&](ar::Comparison* cmp,
                           ValueCache& cache,
                           const std::string& prefix, std::size_t before) -> z3::expr {
    ar::Value* l = cmp->left();
    ar::Value* r = cmp->right();
    // 指针比较（PEQ/PNE/…）或浮点比较：BMC 一期建不了模（pthread_getspecific 等
    // 未建模函数的指针结果），置 unencodable → 不确认（sound，只丢 TP 不引 FP）。
    switch (cmp->predicate()) {
      case ar::Comparison::PEQ:
      case ar::Comparison::PNE:
      case ar::Comparison::PGT:
      case ar::Comparison::PGE:
      case ar::Comparison::PLT:
      case ar::Comparison::PLE:
      case ar::Comparison::FOEQ:
      case ar::Comparison::FOGT:
      case ar::Comparison::FOGE:
      case ar::Comparison::FOLT:
      case ar::Comparison::FOLE:
      case ar::Comparison::FONE:
      case ar::Comparison::FORD:
      case ar::Comparison::FUNO:
      case ar::Comparison::FUEQ:
      case ar::Comparison::FUGT:
      case ar::Comparison::FUGE:
      case ar::Comparison::FULT:
      case ar::Comparison::FULE:
      case ar::Comparison::FUNE:
        record(diagnostics, "assume_predicate", cmp);
        unencodable = true;
        return ctx.bool_val(true);
      default:
        break;
    }
    // A1：编码两个 operand（常量 或 追溯到合法来源的值）。任一无法编码 → 不确认。
    z3::expr x(ctx), y(ctx);
    bool left_ok = encode_value(l, x, cache, prefix, 0, before);
    bool right_ok = encode_value(r, y, cache, prefix, 0, before);
    if (!left_ok || !right_ok) {
      record(diagnostics, "assume_value", cmp);
      unencodable = true;
      return ctx.bool_val(true);
    }
    switch (cmp->predicate()) {
      case ar::Comparison::UIEQ:
      case ar::Comparison::SIEQ:
        return x == y;
      case ar::Comparison::UINE:
      case ar::Comparison::SINE:
        return x != y;
      case ar::Comparison::UIGT:
      case ar::Comparison::SIGT:
        return x > y;
      case ar::Comparison::UIGE:
      case ar::Comparison::SIGE:
        return x >= y;
      case ar::Comparison::UILT:
      case ar::Comparison::SILT:
        return x < y;
      case ar::Comparison::UILE:
      case ar::Comparison::SILE:
        return x <= y;
      default:
        record(diagnostics, "assume_predicate", cmp);
        unencodable = true;
        return ctx.bool_val(true);
    }
  };

  auto assumptions = [&](const std::vector< BmcEvent >& path, ValueCache& cache, const std::string& prefix) {
    for (std::size_t i = 0; i < path.size(); ++i) {
      const auto& e = path[i];
      if (e.kind != EvKind::Assume) continue;
      if (auto* cmp = core::dyn_cast< ar::Comparison >(e.stmt)) {
        s.add(encode_assume(cmp, cache, prefix, i));
      } else if (auto* call = core::dyn_cast< ar::CallBase >(e.stmt)) {
        z3::expr value(ctx);
        if (call->num_arguments() != 1 || !encode_value(call->argument(0), value, cache, prefix, 0, i)) {
          record(diagnostics, "assume_argument", e.stmt);
          unencodable = true;
        } else {
          s.add(value != 0);
        }
      } else {
        record(diagnostics, "assume_statement", e.stmt);
        unencodable = true;
      }
    }
  };
  assumptions(startup, cache_init, "value_init_");
  assumptions(pathA, cache_a, "value_a_");
  assumptions(pathB, cache_b, "value_b_");

  // SMT 同址门：两个访问必须在同一 base 上字节范围重叠。base 相同由 checker 的
  // same_global 门保证，这里判范围重叠；base 不同或提取失败（base==0）则不确认
  // （保持 UNKNOWN）。
  {
    const BmcEvent& ea = pathA.back();
    const BmcEvent& eb = pathB.back();
    // 同址必须可证：base 提取失败（0）或 base 不同 → 缺失同址约束会让两访问被
    // 「假相邻」确认 → FP。保守不确认（UNKNOWN）。
    if (ea.base == 0 || eb.base == 0 || ea.base != eb.base) {
      record(diagnostics, "address_base", ea.base == 0 ? ea.stmt : eb.stmt);
      return false;
    }
    auto encode_offset = [&](const BmcEvent& e, z3::expr& out,
                             ValueCache& cache, const std::string& prefix, std::size_t before) {
      out = ctx.int_val(e.const_off.str().c_str());
      if (e.var_id != 0) {
        z3::expr value(ctx);
        if (!encode_value(reinterpret_cast< ar::InternalVariable* >(e.var_id), value, cache, prefix, 0, e.var_before)) {
          record(diagnostics, "address_value", e.stmt); return false;
        }
        out = out + ctx.int_val(e.coeff.str().c_str()) * value;
      }
      replay_offsets.emplace(&e, out);
      return true;
    };
    auto valid_memory = [&](const std::vector< BmcEvent >& path, ValueCache& cache, const std::string& prefix) {
      for (std::size_t i = 0; i < path.size(); ++i) {
        const auto& e = path[i];
        if (e.kind != EvKind::Read && e.kind != EvKind::Write &&
            !((e.kind == EvKind::Lock || e.kind == EvKind::Unlock) && e.mutex != PSEUDO_ATOMIC_LOCK)) continue;
        core::ZNumber size;
        if (e.width == 0 || !memory_extent(e, size)) {record(diagnostics, "prefix_address", e.stmt); return false;}
        z3::expr offset(ctx);
        if (!encode_offset(e, offset, cache, prefix, i)) return false;
        s.add(offset >= 0 && offset + ctx.int_val(std::to_string(e.width).c_str()) <= ctx.int_val(size.str().c_str()));
      }
      return true;
    };
    if (!valid_memory(pathA, cache_a, "value_a_") || !valid_memory(pathB, cache_b, "value_b_")) return false;
    auto ownership = [&](const std::vector< Section >& sections, const std::vector< BmcEvent >& path,
                         ValueCache& cache, const std::string& prefix) {
      for (const auto& section : sections) {
        if (section.mutex == PSEUDO_ATOMIC_LOCK || section.end == path.size()) continue;
        const auto& lock = path[section.begin]; const auto& unlock = path[section.end];
        if (lock.base != unlock.base) return false;
        z3::expr first(ctx), last(ctx);
        if (!encode_offset(lock, first, cache, prefix, section.begin) ||
            !encode_offset(unlock, last, cache, prefix, section.end)) return false;
        s.add(first == last);
      }
      return true;
    };
    if (!ownership(sections_a, pathA, cache_a, "value_a_") ||
        !ownership(sections_b, pathB, cache_b, "value_b_")) return false;
    for (const auto& a : sections_a) for (const auto& b : sections_b) {
      if (a.mutex == PSEUDO_ATOMIC_LOCK || b.mutex == PSEUDO_ATOMIC_LOCK || (a.reader && b.reader)) continue;
      const auto& lock_a = pathA[a.begin]; const auto& lock_b = pathB[b.begin];
      if (lock_a.base != lock_b.base) continue;
      z3::expr address_a(ctx), address_b(ctx);
      if (!encode_offset(lock_a, address_a, cache_a, "value_a_", a.begin) ||
          !encode_offset(lock_b, address_b, cache_b, "value_b_", b.begin)) return false;
      z3::expr a_first = a.end < nA ? clock[a.end] < clock[nA + b.begin] : ctx.bool_val(false);
      z3::expr b_first = b.end < nB ? clock[nA + b.end] < clock[a.begin] : ctx.bool_val(false);
      s.add(address_a != address_b || a_first || b_first);
    }
    z3::expr off_a(ctx), off_b(ctx);
    if (!encode_offset(ea, off_a, cache_a, "value_a_", nA - 1) || !encode_offset(eb, off_b, cache_b, "value_b_", nB - 1)) return false;
    if (facts != nullptr) {
      if (facts->bounded_a) s.add(off_a >= ctx.int_val(facts->lo_a.str().c_str()) && off_a <= ctx.int_val(facts->hi_a.str().c_str()));
      if (facts->bounded_b) s.add(off_b >= ctx.int_val(facts->lo_b.str().c_str()) && off_b <= ctx.int_val(facts->hi_b.str().c_str()));
      if (facts->bounded_a || facts->bounded_b) record(diagnostics, "ai_candidate_bounds");
    }
    {
      // 字节范围重叠（而非起始 offset 相等）：`x`(u32@[0]) 与 `((u8*)&x)[1]`
      // (u8@[1]) 起始不同但范围重叠 → 同址（overlapping-widths.c FN）。width==0
      // （未知）退化为起始相等（保守，只丢 TP 不引 FP）。
      if (ea.width != 0 && eb.width != 0) {
        z3::expr w_a = ctx.int_val(std::to_string(ea.width).c_str());
        z3::expr w_b = ctx.int_val(std::to_string(eb.width).c_str());
        s.add(off_a < off_b + w_b && off_b < off_a + w_a);
      } else {
        s.add(off_a == off_b);
      }
    }
  }

  // 有无法编码的约束（变量值来源 / 指针 / 浮点比较）→ 保守不确认
  // （UNKNOWN），避免误报。
  if (unencodable) {
    return false;
  }

  // 竞争查询：两个访问（各自路径最后一个事件）能否相邻。
  bool any = false;
  for (int dir = 0; dir < 2; ++dir) {
    // Each query is independent. push/pop switches Z3 to incremental solving,
    // disabling preprocessing needed by some mixed Int/BV guards.
    z3::solver query(ctx);
    query.set("timeout", 1000u);
    query.add(s.assertions());
    if (dir == 0) {
      query.add(clock[nA - 1] == clock[nA + nB - 1] + 1);
    } else {
      query.add(clock[nA + nB - 1] == clock[nA - 1] + 1);
    }
    z3::check_result result = query.check();
    record(diagnostics, result == z3::sat ? "sat" :
                        result == z3::unsat ? "smt_unsat" : "smt_unknown");
    if (result == z3::sat) {
      any = true;
      if (diagnostics != nullptr && diagnostics->model.empty()) {
        auto model = query.get_model();
        diagnostics->model = model.to_string();
        ReplaySnapshot snapshot(funcA->bundle());
        diagnostics->replay.put("schema", 1);
        diagnostics->replay.put("validation_status", "UNVALIDATED");
        diagnostics->replay.put("program", snapshot.program());
        JsonList traces;
        auto export_trace = [&](const std::vector< BmcEvent >& path, const ValueCache& values,
                                const std::string& id, ar::Function* entry, std::size_t start) {
          JsonDict trace; trace.put("id", id); trace.put("entry", entry->name()); JsonList steps;
          if (id != "startup" && entry->name() != "main") {
            const auto& creator = funcA->name() == "main" ? pathA : funcB->name() == "main" ? pathB : startup;
            std::size_t occurrence = id == "b" && funcA == funcB ? 1 : 0;
            for (std::size_t j = 0; j < creator.size(); ++j)
              if (creator[j].kind == EvKind::Create && creator[j].thread == reinterpret_cast< std::uint64_t >(entry)) {
                if (occurrence-- != 0) continue;
                trace.put("creator_trace", funcA->name() == "main" ? "a" : funcB->name() == "main" ? "b" : "startup");
                trace.put("creator_step", j); break;
              }
          }
          for (std::size_t i = 0; i < path.size(); ++i) {
            JsonDict step; step.put("statement", snapshot.statement_id(path[i].stmt));
            if (id != "startup") step.put("clock", Z3_get_numeral_string(ctx, model.eval(clock[start + i], true)));
            auto* result = core::dyn_cast_or_null< ar::InternalVariable >(path[i].stmt->result_or_null());
            if (result != nullptr) {
              auto cached = values.find({result, i});
              if (cached != values.end()) step.put("encoded_result", Z3_get_numeral_string(ctx, model.eval(cached->second, true)));
            }
            const auto& e = path[i];
            if (e.base != 0 || e.local_base != nullptr) {
              auto offset = replay_offsets.find(&e);
              if (offset != replay_offsets.end() || e.var_id == 0) {
                step.put("encoded_address", JsonDict{
                    {"object", snapshot.value(e.base != 0 ? static_cast< ar::Value* >(reinterpret_cast< ar::GlobalVariable* >(e.base)) : e.local_base)},
                    {"offset", offset != replay_offsets.end() ? std::string(Z3_get_numeral_string(ctx, model.eval(offset->second, true))) : e.const_off.str()},
                    {"width", e.width}});
              }
            }
            steps.add(step);
          }
          trace.put("steps", steps); traces.add(trace);
        };
        if (!startup.empty()) export_trace(startup, cache_init, "startup", funcA->bundle()->function_or_null("main"), 0);
        export_trace(pathA, cache_a, "a", funcA, 0);
        export_trace(pathB, cache_b, "b", funcB, nA);
        diagnostics->replay.put("traces", traces);
        diagnostics->replay.put("targets", JsonList(JsonDict{{"trace", "a"}, {"step", nA - 1}},
                                                   JsonDict{{"trace", "b"}, {"step", nB - 1}}));
        for (std::size_t i = 0; i < startup.size(); ++i)
          diagnostics->confirmed_startup.push_back({startup[i].stmt, "startup_" + std::to_string(i)});
        for (std::size_t i = 0; i < nA; ++i)
          diagnostics->confirmed_a.push_back({pathA[i].stmt, Z3_get_numeral_string(ctx, model.eval(clock[i], true))});
        for (std::size_t i = 0; i < nB; ++i)
          diagnostics->confirmed_b.push_back({pathB[i].stmt, Z3_get_numeral_string(ctx, model.eval(clock[nA + i], true))});
      }
    }
    // One SAT direction proves this existential query and preserves its first model.
    if (any) return true;
  }
  return any;
}

}  // namespace bmc
}  // namespace analyzer
}  // namespace ikos
