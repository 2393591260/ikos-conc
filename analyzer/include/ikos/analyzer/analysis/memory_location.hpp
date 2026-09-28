/*******************************************************************************
 *
 * \file
 * \brief MemoryLocation types and MemoryFactory used by the analyses
 *
 * Author: Clement Decoodt
 *
 * Contributors: Maxime Arthaud
 *
 * Contact: ikos@lists.nasa.gov
 *
 * Notices:
 *
 * Copyright (c) 2011-2019 United States Government as represented by the
 * Administrator of the National Aeronautics and Space Administration.
 * All Rights Reserved.
 *
 * Disclaimers:
 *
 * No Warranty: THE SUBJECT SOFTWARE IS PROVIDED "AS IS" WITHOUT ANY WARRANTY OF
 * ANY KIND, EITHER EXPRESSED, IMPLIED, OR STATUTORY, INCLUDING, BUT NOT LIMITED
 * TO, ANY WARRANTY THAT THE SUBJECT SOFTWARE WILL CONFORM TO SPECIFICATIONS,
 * ANY IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE,
 * OR FREEDOM FROM INFRINGEMENT, ANY WARRANTY THAT THE SUBJECT SOFTWARE WILL BE
 * ERROR FREE, OR ANY WARRANTY THAT DOCUMENTATION, IF PROVIDED, WILL CONFORM TO
 * THE SUBJECT SOFTWARE. THIS AGREEMENT DOES NOT, IN ANY MANNER, CONSTITUTE AN
 * ENDORSEMENT BY GOVERNMENT AGENCY OR ANY PRIOR RECIPIENT OF ANY RESULTS,
 * RESULTING DESIGNS, HARDWARE, SOFTWARE PRODUCTS OR ANY OTHER APPLICATIONS
 * RESULTING FROM USE OF THE SUBJECT SOFTWARE.  FURTHER, GOVERNMENT AGENCY
 * DISCLAIMS ALL WARRANTIES AND LIABILITIES REGARDING THIRD-PARTY SOFTWARE,
 * IF PRESENT IN THE ORIGINAL SOFTWARE, AND DISTRIBUTES IT "AS IS."
 *
 * Waiver and Indemnity:  RECIPIENT AGREES TO WAIVE ANY AND ALL CLAIMS AGAINST
 * THE UNITED STATES GOVERNMENT, ITS CONTRACTORS AND SUBCONTRACTORS, AS WELL
 * AS ANY PRIOR RECIPIENT.  IF RECIPIENT'S USE OF THE SUBJECT SOFTWARE RESULTS
 * IN ANY LIABILITIES, DEMANDS, DAMAGES, EXPENSES OR LOSSES ARISING FROM SUCH
 * USE, INCLUDING ANY DAMAGES FROM PRODUCTS BASED ON, OR RESULTING FROM,
 * RECIPIENT'S USE OF THE SUBJECT SOFTWARE, RECIPIENT SHALL INDEMNIFY AND HOLD
 * HARMLESS THE UNITED STATES GOVERNMENT, ITS CONTRACTORS AND SUBCONTRACTORS,
 * AS WELL AS ANY PRIOR RECIPIENT, TO THE EXTENT PERMITTED BY LAW.
 * RECIPIENT'S SOLE REMEDY FOR ANY SUCH MATTER SHALL BE THE IMMEDIATE,
 * UNILATERAL TERMINATION OF THIS AGREEMENT.
 *
 ******************************************************************************/

#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include <boost/thread/shared_mutex.hpp>

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/StringMap.h>
#include <llvm/ADT/StringRef.h>

#include <ikos/core/semantic/dumpable.hpp>
#include <ikos/core/semantic/indexable.hpp>
#include <ikos/core/semantic/memory_location.hpp>

#include <ikos/ar/semantic/function.hpp>
#include <ikos/ar/semantic/statement.hpp>
#include <ikos/ar/semantic/value.hpp>

#include <ikos/analyzer/analysis/call_context.hpp>
#include <ikos/analyzer/support/number.hpp>

namespace ikos {
namespace analyzer {

/// \brief Represents a location in memory
///
/// MemoryLocation is an implementation of the MemoryLocation generic object in
/// the core.
///
/// See ikos/core/semantic/memory_location.hpp
/// It must be indexable (see ikos/core/semantic/indexable.hpp)
/// Is must be dumpable (see ikos/core/semantic/dumpable.hpp)
class MemoryLocation {
public:
  /// \brief kind of the memory location
  enum MemoryLocationKind {
    LocalMemoryKind,
    GlobalMemoryKind,
    FunctionMemoryKind,
    AggregateMemoryKind,
    AbsoluteZeroMemoryKind,
    ArgvMemoryKind,
    LibcErrnoMemoryKind,
    LibcStateMemoryKind,
    DynAllocMemoryKind,
  };

protected:
  /// \brief Kind of the memory location
  MemoryLocationKind _kind;

  /// \brief Stable logical id, assigned at construction time from a global
  /// monotonic counter.
  ///
  /// This is the ORDERING and HASHING key for MemoryLocation* everywhere
  /// (IndexableTraits, std::hash, ...). It deliberately REPLACES the raw
  /// pointer address, whose value varies under ASLR and made the analysis
  /// non-deterministic across runs (region files flapping SAFE↔RACE).
  std::uint64_t _stable_id;

protected:
  /// \brief Protected constructor
  explicit MemoryLocation(MemoryLocationKind kind);

public:
  /// \brief No copy constructor
  MemoryLocation(const MemoryLocation&) = delete;

  /// \brief No move constructor
  MemoryLocation(MemoryLocation&&) = delete;

  /// \brief No copy assignment operator
  MemoryLocation& operator=(const MemoryLocation&) = delete;

  /// \brief No move assignment operator
  MemoryLocation& operator=(MemoryLocation&&) = delete;

  /// \brief Destructor
  virtual ~MemoryLocation();

  /// \brief Return the kind of the object
  MemoryLocationKind kind() const { return this->_kind; }

  /// \brief Return the stable logical id (ASLR-independent ordering key)
  std::uint64_t stable_id() const { return this->_stable_id; }

  /// \brief Dump the memory location, for debugging purpose
  virtual void dump(std::ostream&) const = 0;

}; // end class MemoryLocation

/// \brief Local memory location
class LocalMemoryLocation final : public MemoryLocation {
private:
  /// \brief AR Local Variable
  ar::LocalVariable* _var;

public:
  /// \brief Default constructor
  explicit LocalMemoryLocation(ar::LocalVariable* var);

  /// \brief Get the ar::LocalVariable*
  ar::LocalVariable* local_var() const { return this->_var; }

  /// \brief Dump the memory location, for debugging purpose
  void dump(std::ostream&) const override;

  /// \brief Method for type support (isa, cast, dyn_cast)
  static bool classof(const MemoryLocation* ml) {
    return ml->kind() == LocalMemoryKind;
  }

}; // end class LocalMemoryLocation

/// \brief Global memory location
class GlobalMemoryLocation final : public MemoryLocation {
private:
  /// \brief AR Global Variable
  ar::GlobalVariable* _var;

public:
  /// \brief Default constructor
  explicit GlobalMemoryLocation(ar::GlobalVariable* var);

  /// \brief Get the ar::GlobalVariable*
  ar::GlobalVariable* global_var() const { return this->_var; }

  /// \brief Dump the memory location, for debugging purpose
  void dump(std::ostream&) const override;

  /// \brief Method for type support (isa, cast, dyn_cast)
  static bool classof(const MemoryLocation* ml) {
    return ml->kind() == GlobalMemoryKind;
  }

}; // end class GlobalMemoryLocation

/// \brief Function memory location
class FunctionMemoryLocation final : public MemoryLocation {
private:
  /// \brief Function
  ar::Function* _fun;

public:
  /// \brief Default constructor
  explicit FunctionMemoryLocation(ar::Function* fun);

  /// \brief Get the ar::Function*
  ar::Function* function() const { return this->_fun; }

  /// \brief Dump the memory location, for debugging purpose
  void dump(std::ostream&) const override;

  /// \brief Method for type support (isa, cast, dyn_cast)
  static bool classof(const MemoryLocation* ml) {
    return ml->kind() == FunctionMemoryKind;
  }

}; // end class FunctionMemoryLocation

/// \brief Aggregate memory location
class AggregateMemoryLocation final : public MemoryLocation {
private:
  /// \brief AR Aggregate Internal Variable
  ar::InternalVariable* _var;

public:
  /// \brief Default constructor
  explicit AggregateMemoryLocation(ar::InternalVariable* var);

  /// \brief Get the ar::InternalVariable*
  ar::InternalVariable* internal_var() const { return this->_var; }

  /// \brief Dump the memory location, for debugging purpose
  void dump(std::ostream&) const override;

  /// \brief Method for type support (isa, cast, dyn_cast)
  static bool classof(const MemoryLocation* ml) {
    return ml->kind() == AggregateMemoryKind;
  }

}; // end class AggregateMemoryLocation

/// \brief Absolute zero memory location
class AbsoluteZeroMemoryLocation final : public MemoryLocation {
public:
  /// \brief Default constructor
  AbsoluteZeroMemoryLocation();

  /// \brief Dump the memory location, for debugging purpose
  void dump(std::ostream&) const override;

  /// \brief Method for type support (isa, cast, dyn_cast)
  static bool classof(const MemoryLocation* ml) {
    return ml->kind() == AbsoluteZeroMemoryKind;
  }

}; // end class AbsoluteZeroMemoryLocation

/// \brief Argv memory location
class ArgvMemoryLocation final : public MemoryLocation {
public:
  /// \brief Default constructor
  ArgvMemoryLocation();

  /// \brief Dump the memory location, for debugging purpose
  void dump(std::ostream&) const override;

  /// \brief Method for type support (isa, cast, dyn_cast)
  static bool classof(const MemoryLocation* ml) {
    return ml->kind() == ArgvMemoryKind;
  }

}; // end class ArgvMemoryLocation

/// \brief Libc errno memory location
class LibcErrnoMemoryLocation final : public MemoryLocation {
public:
  /// \brief Default constructor
  LibcErrnoMemoryLocation();

  /// \brief Dump the memory location, for debugging purpose
  void dump(std::ostream&) const override;

  /// \brief Method for type support (isa, cast, dyn_cast)
  static bool classof(const MemoryLocation* ml) {
    return ml->kind() == LibcErrnoMemoryKind;
  }

}; // end class LibcErrnoMemoryLocation

/// \brief Hidden shared state of non-thread-safe libc functions (rand/strtok/
/// strerror/localtime/...). These functions read-modify-write a process-wide
/// internal buffer WITHOUT any lock, so two concurrent calls race on it. This
/// synthetic location gives the data-race checker a single shared anchor to
/// pair such calls against (mirrors Goblint's `ThreadUnsafe` library attr).
class LibcStateMemoryLocation final : public MemoryLocation {
public:
  /// \brief Default constructor
  LibcStateMemoryLocation();

  /// \brief Dump the memory location, for debugging purpose
  void dump(std::ostream&) const override;

  /// \brief Method for type support (isa, cast, dyn_cast)
  static bool classof(const MemoryLocation* ml) {
    return ml->kind() == LibcStateMemoryKind;
  }

}; // end class LibcStateMemoryLocation

/// \brief Dynamic alloc memory location
class DynAllocMemoryLocation final : public MemoryLocation {
private:
  /// \brief Call statement to the allocator (malloc, calloc, etc.)
  ar::CallBase* _call;

  /// \brief Call context
  CallContext* _context;

public:
  /// \brief Default constructor
  explicit DynAllocMemoryLocation(ar::CallBase* call, CallContext* context);

  /// \brief Return the call statement
  ar::CallBase* call() const { return this->_call; }

  /// \brief Return the call context
  CallContext* context() const { return this->_context; }

  /// \brief Dump the memory location, for debugging purpose
  void dump(std::ostream&) const override;

  /// \brief Method for type support (isa, cast, dyn_cast)
  static bool classof(const MemoryLocation* ml) {
    return ml->kind() == DynAllocMemoryKind;
  }

}; // end class DynAllocMemoryLocation

/// \brief Management of memory locations
class MemoryFactory {
private:
  boost::shared_mutex _local_memory_mutex;

  llvm::DenseMap< ar::LocalVariable*, std::unique_ptr< LocalMemoryLocation > >
      _local_memory_map;

  boost::shared_mutex _global_memory_mutex;

  llvm::DenseMap< ar::GlobalVariable*, std::unique_ptr< GlobalMemoryLocation > >
      _global_memory_map;

  boost::shared_mutex _function_memory_mutex;

  llvm::DenseMap< ar::Function*, std::unique_ptr< FunctionMemoryLocation > >
      _function_memory_map;

  boost::shared_mutex _aggregate_memory_mutex;

  llvm::DenseMap< ar::InternalVariable*,
                  std::unique_ptr< AggregateMemoryLocation > >
      _aggregate_memory_map;

  std::unique_ptr< AbsoluteZeroMemoryLocation > _absolute_zero;

  std::unique_ptr< ArgvMemoryLocation > _argv;

  std::unique_ptr< LibcErrnoMemoryLocation > _libc_errno;

  std::unique_ptr< LibcStateMemoryLocation > _libc_state;

  /// \brief Guards the lazy creation of `_libc_state`. The location is only
  /// materialized on first use (a ThreadUnsafe libc call is analyzed), so a
  /// program that never calls rand/strtok/... consumes no extra stable-id and
  /// keeps the rest of the location numbering unchanged.
  std::once_flag _libc_state_once;

  boost::shared_mutex _dyn_alloc_mutex;

  llvm::DenseMap< std::pair< ar::CallBase*, CallContext* >,
                  std::unique_ptr< DynAllocMemoryLocation > >
      _dyn_alloc_map;

public:
  /// \brief Default constructor
  MemoryFactory();

  /// \brief No copy constructor
  MemoryFactory(const MemoryFactory&) = delete;

  /// \brief No move constructor
  MemoryFactory(MemoryFactory&&) = delete;

  /// \brief No copy assignment operator
  MemoryFactory& operator=(const MemoryFactory&) = delete;

  /// \brief No move assignment operator
  MemoryFactory& operator=(MemoryFactory&&) = delete;

  /// \brief Destructor
  ~MemoryFactory();

public:
  /// \brief Get or create a LocalMemoryLocation
  LocalMemoryLocation* get_local(ar::LocalVariable* var);

  /// \brief Get or create a GlobalMemoryLocation
  GlobalMemoryLocation* get_global(ar::GlobalVariable* var);

  /// \brief Get or Create a FunctionMemoryLocation
  FunctionMemoryLocation* get_function(ar::Function* fun);

  /// \brief Get or Create a FunctionMemoryLocation
  FunctionMemoryLocation* get_function(ar::FunctionPointerConstant* cst);

  /// \brief Get or create a AggregateMemoryLocation
  AggregateMemoryLocation* get_aggregate(ar::InternalVariable* var);

  /// \brief Get or create a AbsoluteZeroMemoryLocation
  AbsoluteZeroMemoryLocation* get_absolute_zero();

  /// \brief Get or create a ArgvMemoryLocation
  ArgvMemoryLocation* get_argv();

  /// \brief Get or create a LibcErrnoMemoryLocation
  LibcErrnoMemoryLocation* get_libc_errno();

  /// \brief Get or create a LibcStateMemoryLocation (hidden shared state of
  /// non-thread-safe libc functions)
  LibcStateMemoryLocation* get_libc_state();

  /// \brief Get or create a DynAllocMemoryLocation
  DynAllocMemoryLocation* get_dyn_alloc(ar::CallBase* call,
                                        CallContext* context);

}; // end class MemoryFactory

} // end namespace analyzer
} // end namespace ikos

namespace ikos {
namespace core {

/// \brief Implement IndexableTraits for MemoryLocation*
///
/// The index of MemoryLocation* is its STABLE LOGICAL ID (assigned at
/// construction), NOT the raw pointer address. The pointer address varies
/// under ASLR and previously made every pointer-keyed ordering (patricia
/// tree sets, lock-address keys) non-deterministic across runs.
template <>
struct IndexableTraits< analyzer::MemoryLocation* > {
  static Index index(const analyzer::MemoryLocation* m) {
    return m->stable_id();
  }
};

/// \brief Implement DumpableTraits for MemoryLocation*
template <>
struct DumpableTraits< analyzer::MemoryLocation* > {
  static void dump(std::ostream& o, const analyzer::MemoryLocation* m) {
    m->dump(o);
  }
};

} // end namespace core
} // end namespace ikos

namespace std {

/// \brief Hash for MemoryLocation* based on its stable logical id.
///
/// Replaces the default pointer-address hash so that std::unordered_map /
/// std::unordered_set keyed by MemoryLocation* iterate in a deterministic,
/// ASLR-independent order.
template <>
struct hash< ikos::analyzer::MemoryLocation* > {
  std::size_t operator()(const ikos::analyzer::MemoryLocation* m) const noexcept {
    return static_cast< std::size_t >(m->stable_id());
  }
};

/// \brief Hash for const MemoryLocation* (same stable id).
template <>
struct hash< const ikos::analyzer::MemoryLocation* > {
  std::size_t operator()(const ikos::analyzer::MemoryLocation* m) const noexcept {
    return static_cast< std::size_t >(m->stable_id());
  }
};

} // end namespace std
