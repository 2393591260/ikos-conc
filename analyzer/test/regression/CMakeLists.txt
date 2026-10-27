# Dependencies to run the tests
add_dependencies(build-analyzer-tests ikos-analyzer)

function(add_analysis_test test_name test_directory)
  add_test(NAME "analysis-${test_name}"
           WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/${test_directory}"
           COMMAND ${PYTHON_EXECUTABLE} runtest
             --clang "${CLANG_EXECUTABLE}"
             --ikos-pp "${FRONTEND_LLVM_IKOS_PP_EXECUTABLE}"
             --ikos-analyzer "$<TARGET_FILE:ikos-analyzer>")
endfunction()

add_analysis_test(buffer-overflow boa)
add_analysis_test(division-by-zero dbz)
add_analysis_test(memory mem)
add_analysis_test(null-pointer-dereference null)
add_analysis_test(assert-prover prover)
add_analysis_test(uninitialized-variable uva)
add_analysis_test(unaligned-pointer upa)
add_analysis_test(signed-int-overflow sio)
add_analysis_test(unsigned-int-overflow uio)
add_analysis_test(shift-count shc)
add_analysis_test(pointer-overflow poa)
add_analysis_test(pointer-compare pcmp)
add_analysis_test(function-call fca)
add_analysis_test(double-free dfa)
add_analysis_test(soundness sound)

# These end-to-end tests use the installed frontend and its configured tools.
# Run them after cmake --install; serialize solver-heavy checks on small hosts.
set(CONCURRENCY_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../..")
function(add_concurrency_test test_name)
  add_test(NAME "concurrency-${test_name}"
           COMMAND "${PYTHON_VENV_EXECUTABLE}" ${ARGN})
  set_tests_properties("concurrency-${test_name}" PROPERTIES
                      LABELS concurrency RUN_SERIAL TRUE TIMEOUT 600)
endfunction()
set(CONCURRENCY_IKOS "${CMAKE_INSTALL_PREFIX}/bin/ikos")
add_concurrency_test(race-result "${CONCURRENCY_ROOT}/scripts/test_race_result.py")
add_concurrency_test(strict-coverage "${CONCURRENCY_ROOT}/scripts/test_strict_coverage.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(numeric-thread-argument "${CONCURRENCY_ROOT}/scripts/test_numeric_thread_argument.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(numeric-pointer-null "${CONCURRENCY_ROOT}/scripts/test_numeric_pointer_null.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(detach-coverage "${CONCURRENCY_ROOT}/scripts/test_detach_coverage.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(detach-coverage-lp64 "${CONCURRENCY_ROOT}/scripts/test_detach_coverage.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)
add_concurrency_test(race-rule-trace "${CONCURRENCY_ROOT}/scripts/test_race_rule_trace.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_executable(test-analysis-race-rules race_rules.cpp)
target_include_directories(test-analysis-race-rules PRIVATE
                           "${CONCURRENCY_ROOT}/analyzer/include")
add_dependencies(build-analyzer-tests test-analysis-race-rules)
add_test(NAME concurrency-race-rules
         COMMAND $<TARGET_FILE:test-analysis-race-rules>)
set_tests_properties(concurrency-race-rules PROPERTIES LABELS concurrency)
add_executable(test-analysis-all-access-facts all_access_facts.cpp)
target_include_directories(test-analysis-all-access-facts PRIVATE
                           "${CONCURRENCY_ROOT}/analyzer/include")
add_dependencies(build-analyzer-tests test-analysis-all-access-facts)
add_test(NAME concurrency-all-access-facts
         COMMAND $<TARGET_FILE:test-analysis-all-access-facts>)
set_tests_properties(concurrency-all-access-facts PROPERTIES LABELS concurrency)
add_executable(test-analysis-concurrent-global-env
               concurrent_global_env.cpp
               "${CONCURRENCY_ROOT}/analyzer/src/analysis/variable.cpp"
               "${CONCURRENCY_ROOT}/analyzer/src/exception.cpp")
target_link_libraries(test-analysis-concurrent-global-env
                      ${AR_LIB} ${IKOS_ANALYZER_LLVM_LIBS}
                      ${Boost_LIBRARIES} Threads::Threads)
add_dependencies(build-analyzer-tests test-analysis-concurrent-global-env)
add_test(NAME concurrency-global-env
         COMMAND $<TARGET_FILE:test-analysis-concurrent-global-env>)
set_tests_properties(concurrency-global-env PROPERTIES LABELS concurrency)
add_concurrency_test(modes "${CONCURRENCY_ROOT}/scripts/test_race_modes.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(budgets "${CONCURRENCY_ROOT}/scripts/test_bmc_budgets.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(spawn-arguments "${CONCURRENCY_ROOT}/scripts/test_spawn_arguments.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(shared-heap "${CONCURRENCY_ROOT}/scripts/test_bmc_shared_heap.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(heap-initialization "${CONCURRENCY_ROOT}/scripts/test_heap_pointer_initialization.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(heap-constructors "${CONCURRENCY_ROOT}/scripts/test_heap_constructor_fields.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(heap-initialization-prefix "${CONCURRENCY_ROOT}/scripts/test_heap_initialization_prefix.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(allocation-continuations "${CONCURRENCY_ROOT}/scripts/test_allocation_continuations.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(init-publication "${CONCURRENCY_ROOT}/scripts/test_init_publication.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(constant-thread-completion "${CONCURRENCY_ROOT}/scripts/test_constant_thread_completion.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(first-publication "${CONCURRENCY_ROOT}/scripts/test_first_publication.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(first-publication-lp64 "${CONCURRENCY_ROOT}/scripts/test_first_publication.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)
add_concurrency_test(pthread-key-effects "${CONCURRENCY_ROOT}/scripts/test_pthread_key_effects.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(tls-destructor-effects "${CONCURRENCY_ROOT}/scripts/test_tls_destructor_effects.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(pthread-key-effects-lp64 "${CONCURRENCY_ROOT}/scripts/test_pthread_key_effects.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)
add_concurrency_test(tls-destructor-effects-lp64 "${CONCURRENCY_ROOT}/scripts/test_tls_destructor_effects.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)
add_concurrency_test(self-publication "${CONCURRENCY_ROOT}/scripts/test_self_publication.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(self-publication-lp64 "${CONCURRENCY_ROOT}/scripts/test_self_publication.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)
add_concurrency_test(sc-protocol "${CONCURRENCY_ROOT}/scripts/test_sc_protocol.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(sc-protocol-lp64 "${CONCURRENCY_ROOT}/scripts/test_sc_protocol.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)
add_concurrency_test(mutex-handover "${CONCURRENCY_ROOT}/scripts/test_mutex_handover.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(mutex-handover-lp64 "${CONCURRENCY_ROOT}/scripts/test_mutex_handover.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)
add_concurrency_test(heap-handoff "${CONCURRENCY_ROOT}/scripts/test_heap_handoff.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(heap-handoff-lp64 "${CONCURRENCY_ROOT}/scripts/test_heap_handoff.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)
add_concurrency_test(arena-reservation "${CONCURRENCY_ROOT}/scripts/test_arena_reservation.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(arena-reservation-lp64 "${CONCURRENCY_ROOT}/scripts/test_arena_reservation.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)
add_concurrency_test(symbolic-arena "${CONCURRENCY_ROOT}/scripts/test_symbolic_arena.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(symbolic-arena-lp64 "${CONCURRENCY_ROOT}/scripts/test_symbolic_arena.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)
add_concurrency_test(token-once "${CONCURRENCY_ROOT}/scripts/test_token_once.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(token-once-lp64 "${CONCURRENCY_ROOT}/scripts/test_token_once.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)
add_concurrency_test(token-arena "${CONCURRENCY_ROOT}/scripts/test_token_arena.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(token-arena-lp64 "${CONCURRENCY_ROOT}/scripts/test_token_arena.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)
add_concurrency_test(scanf-errno "${CONCURRENCY_ROOT}/scripts/test_scanf_errno.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(scanf-errno-lp64 "${CONCURRENCY_ROOT}/scripts/test_scanf_errno.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)
add_concurrency_test(runtime-stdio "${CONCURRENCY_ROOT}/scripts/test_runtime_stdio.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(runtime-stdio-lp64 "${CONCURRENCY_ROOT}/scripts/test_runtime_stdio.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)
add_concurrency_test(scalar-scan "${CONCURRENCY_ROOT}/scripts/test_scalar_scan.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(scalar-scan-lp64 "${CONCURRENCY_ROOT}/scripts/test_scalar_scan.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)
add_concurrency_test(freeze-array "${CONCURRENCY_ROOT}/scripts/test_freeze_array.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(ticket-ownership "${CONCURRENCY_ROOT}/scripts/test_ticket_ownership.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(ticket-ownership-lp64 "${CONCURRENCY_ROOT}/scripts/test_ticket_ownership.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)
add_concurrency_test(guarded-publication "${CONCURRENCY_ROOT}/scripts/test_guarded_publication.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(guarded-publication-lp64 "${CONCURRENCY_ROOT}/scripts/test_guarded_publication.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)
add_concurrency_test(posix-semaphore "${CONCURRENCY_ROOT}/scripts/test_posix_semaphore.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(bmc-semaphore "${CONCURRENCY_ROOT}/scripts/test_bmc_semaphore.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(bmc-semaphore-lp64 "${CONCURRENCY_ROOT}/scripts/test_bmc_semaphore.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)
# This direct replay suite never launches an analyzer. Source fixtures require
# an explicit --ikos diagnostic run and do not establish SAFE/recovery claims.
add_concurrency_test(bmc-recursive-replay "${CONCURRENCY_ROOT}/scripts/test_bmc_recursive_mutex.py"
                     --bits 32)
add_concurrency_test(bmc-recursive-replay-lp64 "${CONCURRENCY_ROOT}/scripts/test_bmc_recursive_mutex.py"
                     --bits 64)
add_concurrency_test(bmc-mutex-lifetime "${CONCURRENCY_ROOT}/scripts/test_bmc_mutex_lifetime.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(bmc-main-stack-mutex "${CONCURRENCY_ROOT}/scripts/test_bmc_main_stack_mutex.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(bmc-executed-offset "${CONCURRENCY_ROOT}/scripts/test_bmc_executed_offset.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(bmc-executed-offset-lp64 "${CONCURRENCY_ROOT}/scripts/test_bmc_executed_offset.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)
add_concurrency_test(bmc-main-vla "${CONCURRENCY_ROOT}/scripts/test_bmc_main_vla.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(shared-node-graph "${CONCURRENCY_ROOT}/scripts/test_shared_node_graph.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(binary-token "${CONCURRENCY_ROOT}/scripts/test_binary_token.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(binary-token-lp64 "${CONCURRENCY_ROOT}/scripts/test_binary_token.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)
add_concurrency_test(direct-token "${CONCURRENCY_ROOT}/scripts/test_direct_token.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(direct-once "${CONCURRENCY_ROOT}/scripts/test_direct_once.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(direct-once-lp64 "${CONCURRENCY_ROOT}/scripts/test_direct_once.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)
add_concurrency_test(zero-guard-arena "${CONCURRENCY_ROOT}/scripts/test_zero_guard_arena.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(zero-guard-arena-lp64 "${CONCURRENCY_ROOT}/scripts/test_zero_guard_arena.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)
add_concurrency_test(native-arena "${CONCURRENCY_ROOT}/scripts/test_native_arena.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(native-arena-lp64 "${CONCURRENCY_ROOT}/scripts/test_native_arena.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)
add_concurrency_test(once-callbacks "${CONCURRENCY_ROOT}/scripts/test_once_callbacks.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(once-initialization "${CONCURRENCY_ROOT}/scripts/test_once_initialization.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(once-initialization-lp64 "${CONCURRENCY_ROOT}/scripts/test_once_initialization.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)
add_concurrency_test(thread-private-heap "${CONCURRENCY_ROOT}/scripts/test_thread_private_heap.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(thread-private-heap-lp64 "${CONCURRENCY_ROOT}/scripts/test_thread_private_heap.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)
add_concurrency_test(calloc-overflow "${CONCURRENCY_ROOT}/scripts/test_calloc_overflow.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(region-ownership "${CONCURRENCY_ROOT}/scripts/test_region_ownership.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(region-ownership-lp64 "${CONCURRENCY_ROOT}/scripts/test_region_ownership.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)
add_concurrency_test(head-publication "${CONCURRENCY_ROOT}/scripts/test_head_publication.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(array-slices "${CONCURRENCY_ROOT}/scripts/test_array_slices.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(heap-pthread-initialization "${CONCURRENCY_ROOT}/scripts/test_heap_pthread_initialization.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(pthread-array-initialization "${CONCURRENCY_ROOT}/scripts/test_pthread_array_initialization.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(readonly-heap-reads "${CONCURRENCY_ROOT}/scripts/test_readonly_heap_reads.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(readonly-heap-reads-lp64 "${CONCURRENCY_ROOT}/scripts/test_readonly_heap_reads.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)
add_concurrency_test(readonly-loop-snapshots "${CONCURRENCY_ROOT}/scripts/test_readonly_loop_snapshots.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(readonly-loop-snapshots-lp64 "${CONCURRENCY_ROOT}/scripts/test_readonly_loop_snapshots.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)
add_concurrency_test(initialized-pointer-fields "${CONCURRENCY_ROOT}/scripts/test_initialized_pointer_fields.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(allocator-inlining "${CONCURRENCY_ROOT}/scripts/test_allocator_inlining.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(bucket-list-ownership "${CONCURRENCY_ROOT}/scripts/test_bucket_list_ownership.py"
  --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(protected-protocol "${CONCURRENCY_ROOT}/scripts/test_protected_protocol.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(protected-protocol-lp64 "${CONCURRENCY_ROOT}/scripts/test_protected_protocol.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)
add_concurrency_test(scalar-thread-completion "${CONCURRENCY_ROOT}/scripts/test_scalar_thread_completion.py"
  --ikos "${CONCURRENCY_IKOS}")

add_concurrency_test(thread-lifecycle "${CONCURRENCY_ROOT}/scripts/test_thread_lifecycle.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(thread-lifecycle-lp64 "${CONCURRENCY_ROOT}/scripts/test_thread_lifecycle.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)
add_concurrency_test(boolean-partitioning "${CONCURRENCY_ROOT}/scripts/test_boolean_partitioning.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(protected-field-values "${CONCURRENCY_ROOT}/scripts/test_protected_field_values.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(protected-field-values-lp64 "${CONCURRENCY_ROOT}/scripts/test_protected_field_values.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)
add_concurrency_test(mutex-array-effects "${CONCURRENCY_ROOT}/scripts/test_mutex_array_effects.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(mutex-array-effects-lp64 "${CONCURRENCY_ROOT}/scripts/test_mutex_array_effects.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)
add_concurrency_test(race-cooperation "${CONCURRENCY_ROOT}/scripts/test_race_cooperation.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(race-cooperation-lp64 "${CONCURRENCY_ROOT}/scripts/test_race_cooperation.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)
add_concurrency_test(preflight-memory-ranges "${CONCURRENCY_ROOT}/scripts/test_preflight_memory_ranges.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(preflight-memory-ranges-lp64 "${CONCURRENCY_ROOT}/scripts/test_preflight_memory_ranges.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)
add_concurrency_test(nested-array-ownership "${CONCURRENCY_ROOT}/scripts/test_nested_array_ownership.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(nested-array-ownership-lp64 "${CONCURRENCY_ROOT}/scripts/test_nested_array_ownership.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)
add_concurrency_test(native-thread-handles "${CONCURRENCY_ROOT}/scripts/test_native_thread_handles.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(native-scalar-thread-handles "${CONCURRENCY_ROOT}/scripts/test_native_scalar_thread_handles.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 32)
add_concurrency_test(native-scalar-thread-handles-lp64 "${CONCURRENCY_ROOT}/scripts/test_native_scalar_thread_handles.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)
add_concurrency_test(native-thread-handles-lp64 "${CONCURRENCY_ROOT}/scripts/test_native_thread_handles.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)
foreach(bits 32 64)
  add_concurrency_test(calloc-${bits} "${CONCURRENCY_ROOT}/scripts/test_bmc_calloc.py"
                       --ikos "${CONCURRENCY_IKOS}" --bits ${bits})
endforeach()
foreach(bits 32 64)
  add_concurrency_test(malloc-helpers-${bits} "${CONCURRENCY_ROOT}/scripts/test_malloc_helpers.py"
                       --ikos "${CONCURRENCY_IKOS}" --bits ${bits})
endforeach()
foreach(bits 32 64)
  add_concurrency_test(native-mechanisms-${bits} "${CONCURRENCY_ROOT}/scripts/test_native_mechanisms.py"
                       --ikos "${CONCURRENCY_IKOS}" --bits ${bits})
endforeach()
foreach(bits 32 64)
  add_concurrency_test(native-abi-${bits} "${CONCURRENCY_ROOT}/scripts/test_native_abi.py"
                       --ikos "${CONCURRENCY_IKOS}" --bits ${bits})
endforeach()
add_concurrency_test(ffs "${CONCURRENCY_ROOT}/scripts/test_bmc_ffs.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(private-array-zero "${CONCURRENCY_ROOT}/scripts/test_private_array_zero.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(atomic-completion "${CONCURRENCY_ROOT}/scripts/test_bmc_atomic_completion.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(heap-completion "${CONCURRENCY_ROOT}/scripts/test_bmc_heap_completion.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(prefix-search "${CONCURRENCY_ROOT}/scripts/test_bmc_prefix_search.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(sibling-atomic "${CONCURRENCY_ROOT}/scripts/test_bmc_sibling_atomic.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(atomic-unordered "${CONCURRENCY_ROOT}/scripts/test_bmc_atomic_unordered.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(atomic-unordered-lp64 "${CONCURRENCY_ROOT}/scripts/test_bmc_atomic_unordered.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)
foreach(bits 32 64)
  add_concurrency_test(p0-${bits}
    "${CONCURRENCY_ROOT}/docs/review-repros/bmc-2026-10-08/run.py"
    --ikos "${CONCURRENCY_IKOS}" --bits ${bits})
endforeach()
foreach(suite address values correctness auxiliary)
  add_concurrency_test(${suite} "${CMAKE_CURRENT_SOURCE_DIR}/bmc-${suite}/runtest"
                       --ikos "${CONCURRENCY_IKOS}")
  add_concurrency_test(${suite}-lp64 "${CMAKE_CURRENT_SOURCE_DIR}/bmc-${suite}/runtest"
                       --ikos "${CONCURRENCY_IKOS}" --bits 64)
endforeach()
add_concurrency_test(execution "${CONCURRENCY_ROOT}/scripts/test_bmc_execution.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(witness "${CONCURRENCY_ROOT}/scripts/test_svcomp_witness.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(adapter "${CONCURRENCY_ROOT}/scripts/test_svcomp_adapter.py")

add_concurrency_test(barrier-phases "${CONCURRENCY_ROOT}/scripts/test_barrier_phases.py"
                     --ikos "${CONCURRENCY_IKOS}")

add_concurrency_test(locked-creation "${CONCURRENCY_ROOT}/scripts/test_locked_creation.py"
                     --ikos "${CONCURRENCY_IKOS}")

add_executable(test-analysis-closed-integer-fold closed_integer_fold.cpp
               "${CONCURRENCY_ROOT}/analyzer/src/analysis/variable.cpp"
               "${CONCURRENCY_ROOT}/analyzer/src/exception.cpp")
target_link_libraries(test-analysis-closed-integer-fold
                      ${AR_LIB} ${IKOS_ANALYZER_LLVM_LIBS}
                      ${Boost_LIBRARIES} Threads::Threads)
add_dependencies(build-analyzer-tests test-analysis-closed-integer-fold)
add_test(NAME concurrency-closed-integer-fold
         COMMAND $<TARGET_FILE:test-analysis-closed-integer-fold>)
set_tests_properties(concurrency-closed-integer-fold PROPERTIES LABELS concurrency)

add_executable(test-analysis-source-program-facts source_program_facts.cpp
               "${CONCURRENCY_ROOT}/analyzer/src/analysis/source_program_facts.cpp")
target_link_libraries(test-analysis-source-program-facts
                      ${AR_LIB} ${IKOS_ANALYZER_LLVM_LIBS}
                      ${Boost_LIBRARIES} Threads::Threads)
add_dependencies(build-analyzer-tests test-analysis-source-program-facts)
add_test(NAME concurrency-source-program-facts
         COMMAND $<TARGET_FILE:test-analysis-source-program-facts>)
set_tests_properties(concurrency-source-program-facts PROPERTIES LABELS concurrency)

add_concurrency_test(readonly-abi "${CONCURRENCY_ROOT}/scripts/test_readonly_abi.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(readonly-abi-lp64 "${CONCURRENCY_ROOT}/scripts/test_readonly_abi.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)

add_concurrency_test(array-slice-occurrences "${CONCURRENCY_ROOT}/scripts/test_array_slice_occurrences.py"
                     --ikos "${CONCURRENCY_IKOS}")

add_concurrency_test(heap-pthread-initialization-lp64 "${CONCURRENCY_ROOT}/scripts/test_heap_pthread_initialization.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)

add_concurrency_test(pthread-array-initialization-lp64 "${CONCURRENCY_ROOT}/scripts/test_pthread_array_initialization.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)

add_concurrency_test(pthread-initialization-abi "${CONCURRENCY_ROOT}/scripts/test_pthread_initialization_abi.py"
                     --ikos "${CONCURRENCY_IKOS}")

add_concurrency_test(pthread-initialization-abi-lp64 "${CONCURRENCY_ROOT}/scripts/test_pthread_initialization_abi.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)

add_concurrency_test(source-call-abi "${CONCURRENCY_ROOT}/scripts/test_source_call_abi.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(source-call-abi-lp64 "${CONCURRENCY_ROOT}/scripts/test_source_call_abi.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)

add_concurrency_test(array-slices-lp64 "${CONCURRENCY_ROOT}/scripts/test_array_slices.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)

add_concurrency_test(array-slice-occurrences-lp64 "${CONCURRENCY_ROOT}/scripts/test_array_slice_occurrences.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)

add_concurrency_test(array-slice-constant-geps "${CONCURRENCY_ROOT}/scripts/test_array_slice_constant_geps.py"
                     --ikos "${CONCURRENCY_IKOS}")

add_concurrency_test(array-slice-constant-geps-lp64 "${CONCURRENCY_ROOT}/scripts/test_array_slice_constant_geps.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)

add_concurrency_test(shared-node-graph-lp64 "${CONCURRENCY_ROOT}/scripts/test_shared_node_graph.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)

add_concurrency_test(bucket-list-ownership-lp64 "${CONCURRENCY_ROOT}/scripts/test_bucket_list_ownership.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)

add_concurrency_test(bucket-abi "${CONCURRENCY_ROOT}/scripts/test_bucket_abi.py"
                     --ikos "${CONCURRENCY_IKOS}")

add_concurrency_test(bucket-abi-lp64 "${CONCURRENCY_ROOT}/scripts/test_bucket_abi.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)


add_concurrency_test(bucket-abi-attributes "${CONCURRENCY_ROOT}/scripts/test_bucket_abi_attributes.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(bucket-abi-attributes-lp64 "${CONCURRENCY_ROOT}/scripts/test_bucket_abi_attributes.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)

add_concurrency_test(allocator-inlining-lp64 "${CONCURRENCY_ROOT}/scripts/test_allocator_inlining.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)

add_concurrency_test(atomic-completion-lp64 "${CONCURRENCY_ROOT}/scripts/test_bmc_atomic_completion.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)

add_concurrency_test(ffs-lp64 "${CONCURRENCY_ROOT}/scripts/test_bmc_ffs.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)

add_concurrency_test(heap-completion-lp64 "${CONCURRENCY_ROOT}/scripts/test_bmc_heap_completion.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)

add_concurrency_test(bmc-main-stack-mutex-lp64 "${CONCURRENCY_ROOT}/scripts/test_bmc_main_stack_mutex.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)

add_concurrency_test(bmc-main-vla-lp64 "${CONCURRENCY_ROOT}/scripts/test_bmc_main_vla.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)

add_concurrency_test(bmc-mutex-lifetime-lp64 "${CONCURRENCY_ROOT}/scripts/test_bmc_mutex_lifetime.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)

add_concurrency_test(prefix-search-lp64 "${CONCURRENCY_ROOT}/scripts/test_bmc_prefix_search.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)

add_concurrency_test(sibling-atomic-lp64 "${CONCURRENCY_ROOT}/scripts/test_bmc_sibling_atomic.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)

add_concurrency_test(boolean-partitioning-lp64 "${CONCURRENCY_ROOT}/scripts/test_boolean_partitioning.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)

add_concurrency_test(calloc-overflow-lp64 "${CONCURRENCY_ROOT}/scripts/test_calloc_overflow.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)

add_concurrency_test(direct-token-lp64 "${CONCURRENCY_ROOT}/scripts/test_direct_token.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)

add_concurrency_test(freeze-array-lp64 "${CONCURRENCY_ROOT}/scripts/test_freeze_array.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)

add_concurrency_test(head-publication-lp64 "${CONCURRENCY_ROOT}/scripts/test_head_publication.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)

add_concurrency_test(heap-constructors-lp64 "${CONCURRENCY_ROOT}/scripts/test_heap_constructor_fields.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)

add_concurrency_test(heap-initialization-prefix-lp64 "${CONCURRENCY_ROOT}/scripts/test_heap_initialization_prefix.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)

add_concurrency_test(heap-initialization-lp64 "${CONCURRENCY_ROOT}/scripts/test_heap_pointer_initialization.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)

add_concurrency_test(initialized-pointer-fields-lp64 "${CONCURRENCY_ROOT}/scripts/test_initialized_pointer_fields.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)

add_concurrency_test(numeric-pointer-null-lp64 "${CONCURRENCY_ROOT}/scripts/test_numeric_pointer_null.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)

add_concurrency_test(numeric-thread-argument-lp64 "${CONCURRENCY_ROOT}/scripts/test_numeric_thread_argument.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)

add_concurrency_test(private-array-zero-lp64 "${CONCURRENCY_ROOT}/scripts/test_private_array_zero.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)

add_concurrency_test(race-rule-trace-lp64 "${CONCURRENCY_ROOT}/scripts/test_race_rule_trace.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)

add_concurrency_test(spawn-arguments-lp64 "${CONCURRENCY_ROOT}/scripts/test_spawn_arguments.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)

add_concurrency_test(strict-coverage-lp64 "${CONCURRENCY_ROOT}/scripts/test_strict_coverage.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)

add_concurrency_test(budgets-lp64 "${CONCURRENCY_ROOT}/scripts/test_bmc_budgets.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)

add_concurrency_test(modes-lp64 "${CONCURRENCY_ROOT}/scripts/test_race_modes.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)

add_concurrency_test(execution-lp64 "${CONCURRENCY_ROOT}/scripts/test_bmc_execution.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)

add_concurrency_test(results "${CONCURRENCY_ROOT}/scripts/test_bmc_results.py"
                     --ikos "${CONCURRENCY_IKOS}")

add_concurrency_test(ablate-bmc "${CONCURRENCY_ROOT}/scripts/test_ablate_bmc.py")

add_concurrency_test(reference-unknowns "${CONCURRENCY_ROOT}/scripts/test_reference_unknowns.py")

add_concurrency_test(witness-export "${CONCURRENCY_ROOT}/scripts/test_bmc_witnesses.py")

add_concurrency_test(bmc-fact-switches "${CONCURRENCY_ROOT}/scripts/test_bmc_fact_switches.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 32)

add_concurrency_test(bmc-fact-switches-lp64 "${CONCURRENCY_ROOT}/scripts/test_bmc_fact_switches.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)

add_concurrency_test(pair-exclusion "${CONCURRENCY_ROOT}/scripts/test_pair_exclusion.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 32)

add_concurrency_test(pair-exclusion-lp64 "${CONCURRENCY_ROOT}/scripts/test_pair_exclusion.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)

add_executable(test-analysis-pair-exclusion pair_exclusion.cpp)
target_include_directories(test-analysis-pair-exclusion PRIVATE
                           "${CONCURRENCY_ROOT}/analyzer/include")
target_link_libraries(test-analysis-pair-exclusion ${Z3_LIB} ${GMPXX_LIB} ${GMP_LIB})
add_dependencies(build-analyzer-tests test-analysis-pair-exclusion)
add_test(NAME concurrency-pair-exclusion-unit
         COMMAND $<TARGET_FILE:test-analysis-pair-exclusion>)
set_tests_properties(concurrency-pair-exclusion-unit PROPERTIES LABELS concurrency)

add_concurrency_test(later-accesses "${CONCURRENCY_ROOT}/scripts/test_bmc_later_accesses.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 32)

add_concurrency_test(later-accesses-lp64 "${CONCURRENCY_ROOT}/scripts/test_bmc_later_accesses.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)

add_concurrency_test(finite-coverage "${CONCURRENCY_ROOT}/scripts/test_bmc_finite_coverage.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 32)

add_concurrency_test(finite-coverage-lp64 "${CONCURRENCY_ROOT}/scripts/test_bmc_finite_coverage.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)

add_concurrency_test(thread-argument-windows "${CONCURRENCY_ROOT}/scripts/test_thread_argument_windows.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 32)

add_concurrency_test(thread-argument-windows-lp64 "${CONCURRENCY_ROOT}/scripts/test_thread_argument_windows.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)

add_concurrency_test(default-thread-argument-windows "${CONCURRENCY_ROOT}/scripts/test_default_thread_argument_windows.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 32)
add_concurrency_test(default-thread-argument-windows-lp64 "${CONCURRENCY_ROOT}/scripts/test_default_thread_argument_windows.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)

add_concurrency_test(indexed-precreate "${CONCURRENCY_ROOT}/scripts/test_indexed_precreate.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 32)
add_concurrency_test(indexed-precreate-lp64 "${CONCURRENCY_ROOT}/scripts/test_indexed_precreate.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)

add_executable(test-analysis-thread-argument-window-consumer thread_argument_window_consumer.cpp)
target_include_directories(test-analysis-thread-argument-window-consumer PRIVATE "${CONCURRENCY_ROOT}/analyzer/include")
target_link_libraries(test-analysis-thread-argument-window-consumer ${GMPXX_LIB} ${GMP_LIB} ${APRON_LIBRARIES} LLVMSupport Threads::Threads)
add_dependencies(build-analyzer-tests test-analysis-thread-argument-window-consumer)
add_test(NAME concurrency-thread-argument-window-consumer COMMAND $<TARGET_FILE:test-analysis-thread-argument-window-consumer>)
set_tests_properties(concurrency-thread-argument-window-consumer PROPERTIES LABELS concurrency)

add_executable(test-analysis-thread-argument-window-native thread_argument_window_native.cpp)
target_include_directories(test-analysis-thread-argument-window-native PRIVATE "${CONCURRENCY_ROOT}/analyzer/include")
target_link_libraries(test-analysis-thread-argument-window-native ${GMPXX_LIB} ${GMP_LIB} ${APRON_LIBRARIES} LLVMSupport Threads::Threads)
add_dependencies(build-analyzer-tests test-analysis-thread-argument-window-native)
add_test(NAME concurrency-thread-argument-window-native COMMAND $<TARGET_FILE:test-analysis-thread-argument-window-native>)
set_tests_properties(concurrency-thread-argument-window-native PROPERTIES LABELS concurrency)

add_executable(test-analysis-thread-argument-window-projection thread_argument_window_projection.cpp)
target_include_directories(test-analysis-thread-argument-window-projection PRIVATE "${CONCURRENCY_ROOT}/analyzer/include")
target_link_libraries(test-analysis-thread-argument-window-projection ${GMPXX_LIB} ${GMP_LIB} ${APRON_LIBRARIES} LLVMSupport Threads::Threads)
add_dependencies(build-analyzer-tests test-analysis-thread-argument-window-projection)
add_test(NAME concurrency-thread-argument-window-projection COMMAND $<TARGET_FILE:test-analysis-thread-argument-window-projection>)
set_tests_properties(concurrency-thread-argument-window-projection PROPERTIES LABELS concurrency)

# Maintained bounded concurrency completeness controls.
add_concurrency_test(startup-provider "${CONCURRENCY_ROOT}/scripts/test_bmc_startup_provider.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 32)
add_concurrency_test(startup-provider-lp64 "${CONCURRENCY_ROOT}/scripts/test_bmc_startup_provider.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)
add_concurrency_test(heap-generations "${CONCURRENCY_ROOT}/scripts/test_bmc_heap_generations.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 32)
add_concurrency_test(heap-generations-lp64 "${CONCURRENCY_ROOT}/scripts/test_bmc_heap_generations.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)
add_concurrency_test(heap-canonical-events "${CONCURRENCY_ROOT}/scripts/test_bmc_heap_canonical_events.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 32)
add_concurrency_test(heap-canonical-events-lp64 "${CONCURRENCY_ROOT}/scripts/test_bmc_heap_canonical_events.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)
add_concurrency_test(architecture-combinations "${CONCURRENCY_ROOT}/scripts/test_bmc_architecture_combinations.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 32)
add_concurrency_test(architecture-combinations-lp64 "${CONCURRENCY_ROOT}/scripts/test_bmc_architecture_combinations.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)
add_concurrency_test(combination-oracle "${CONCURRENCY_ROOT}/scripts/test_bmc_combination_oracle.py"
)

add_concurrency_test(bmc-creator-release "${CONCURRENCY_ROOT}/scripts/test_bmc_creator_release.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 32)
add_concurrency_test(bmc-creator-release-lp64 "${CONCURRENCY_ROOT}/scripts/test_bmc_creator_release.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)

add_concurrency_test(bmc-once-physical "${CONCURRENCY_ROOT}/scripts/test_bmc_once_physical.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 32 --broken-premises --finite-controls --export-source-witness)
add_concurrency_test(bmc-once-physical-lp64 "${CONCURRENCY_ROOT}/scripts/test_bmc_once_physical.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64 --broken-premises --finite-controls --export-source-witness)

add_concurrency_test(bmc-barrier-execution "${CONCURRENCY_ROOT}/scripts/test_bmc_barrier_execution.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 32)
add_concurrency_test(bmc-barrier-execution-lp64 "${CONCURRENCY_ROOT}/scripts/test_bmc_barrier_execution.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)

add_concurrency_test(bmc-finite-ladder "${CONCURRENCY_ROOT}/scripts/test_bmc_finite_ladder.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 32)
add_concurrency_test(bmc-finite-ladder-lp64 "${CONCURRENCY_ROOT}/scripts/test_bmc_finite_ladder.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)

add_concurrency_test(nested-joined-scope "${CONCURRENCY_ROOT}/scripts/test_nested_join_scope.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 32)
add_concurrency_test(nested-joined-scope-lp64 "${CONCURRENCY_ROOT}/scripts/test_nested_join_scope.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)

add_concurrency_test(numeric-index-windows "${CONCURRENCY_ROOT}/scripts/test_numeric_index_windows.py" --bits 32)
add_concurrency_test(numeric-index-windows-lp64 "${CONCURRENCY_ROOT}/scripts/test_numeric_index_windows.py" --bits 64)

add_concurrency_test(barrier-capacity "${CONCURRENCY_ROOT}/scripts/test_barrier_capacity_controls.py" --bits 32)
add_concurrency_test(barrier-capacity-lp64 "${CONCURRENCY_ROOT}/scripts/test_barrier_capacity_controls.py" --bits 64)

add_concurrency_test(indexed-join-family "${CONCURRENCY_ROOT}/scripts/test_indexed_join_family.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 32)

foreach(bits 32 64)
  add_concurrency_test(cmpxchg-values-${bits} "${CONCURRENCY_ROOT}/scripts/test_cmpxchg_values.py"
                       --ikos "${CONCURRENCY_IKOS}" --bits ${bits})
endforeach()

add_concurrency_test(recursive-mutex-finite "${CONCURRENCY_ROOT}/scripts/test_recursive_mutex_finite.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(recursive-mutex-finite-lp64 "${CONCURRENCY_ROOT}/scripts/test_recursive_mutex_finite.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64)

add_concurrency_test(recursive-mutex-bmc "${CONCURRENCY_ROOT}/scripts/test_recursive_mutex_finite.py"
                     --ikos "${CONCURRENCY_IKOS}" --mode bmc --case 0 --case 1 --case 2 --case 3)
add_concurrency_test(recursive-mutex-bmc-lp64 "${CONCURRENCY_ROOT}/scripts/test_recursive_mutex_finite.py"
                     --ikos "${CONCURRENCY_IKOS}" --bits 64 --mode bmc --case 0 --case 1 --case 2 --case 3)

add_concurrency_test(recursive-mutex-replay "${CONCURRENCY_ROOT}/scripts/test_bmc_recursive_mutex.py")
