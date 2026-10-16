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
add_concurrency_test(first-publication "${CONCURRENCY_ROOT}/scripts/test_first_publication.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(pthread-key-effects "${CONCURRENCY_ROOT}/scripts/test_pthread_key_effects.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(tls-destructor-effects "${CONCURRENCY_ROOT}/scripts/test_tls_destructor_effects.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(sc-protocol "${CONCURRENCY_ROOT}/scripts/test_sc_protocol.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(mutex-handover "${CONCURRENCY_ROOT}/scripts/test_mutex_handover.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(heap-handoff "${CONCURRENCY_ROOT}/scripts/test_heap_handoff.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(arena-reservation "${CONCURRENCY_ROOT}/scripts/test_arena_reservation.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(token-once "${CONCURRENCY_ROOT}/scripts/test_token_once.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(token-arena "${CONCURRENCY_ROOT}/scripts/test_token_arena.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(binary-token "${CONCURRENCY_ROOT}/scripts/test_binary_token.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(direct-token "${CONCURRENCY_ROOT}/scripts/test_direct_token.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(direct-once "${CONCURRENCY_ROOT}/scripts/test_direct_once.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(zero-guard-arena "${CONCURRENCY_ROOT}/scripts/test_zero_guard_arena.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(native-arena "${CONCURRENCY_ROOT}/scripts/test_native_arena.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(once-initialization "${CONCURRENCY_ROOT}/scripts/test_once_initialization.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(thread-private-heap "${CONCURRENCY_ROOT}/scripts/test_thread_private_heap.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(calloc-overflow "${CONCURRENCY_ROOT}/scripts/test_calloc_overflow.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(region-ownership "${CONCURRENCY_ROOT}/scripts/test_region_ownership.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(boolean-partitioning "${CONCURRENCY_ROOT}/scripts/test_boolean_partitioning.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(mutex-array-effects "${CONCURRENCY_ROOT}/scripts/test_mutex_array_effects.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(nested-array-ownership "${CONCURRENCY_ROOT}/scripts/test_nested_array_ownership.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(native-thread-handles "${CONCURRENCY_ROOT}/scripts/test_native_thread_handles.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(ffs "${CONCURRENCY_ROOT}/scripts/test_bmc_ffs.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(atomic-completion "${CONCURRENCY_ROOT}/scripts/test_bmc_atomic_completion.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(heap-completion "${CONCURRENCY_ROOT}/scripts/test_bmc_heap_completion.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(prefix-search "${CONCURRENCY_ROOT}/scripts/test_bmc_prefix_search.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(sibling-atomic "${CONCURRENCY_ROOT}/scripts/test_bmc_sibling_atomic.py"
                     --ikos "${CONCURRENCY_IKOS}")
foreach(bits 32)
  add_concurrency_test(p0-${bits}
    "${CONCURRENCY_ROOT}/docs/review-repros/bmc-2026-10-08/run.py"
    --ikos "${CONCURRENCY_IKOS}" --bits ${bits})
endforeach()
foreach(suite address values correctness auxiliary)
  add_concurrency_test(${suite} "${CMAKE_CURRENT_SOURCE_DIR}/bmc-${suite}/runtest"
                       --ikos "${CONCURRENCY_IKOS}")
endforeach()
add_concurrency_test(execution "${CONCURRENCY_ROOT}/scripts/test_bmc_execution.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(witness "${CONCURRENCY_ROOT}/scripts/test_svcomp_witness.py"
                     --ikos "${CONCURRENCY_IKOS}")
add_concurrency_test(adapter "${CONCURRENCY_ROOT}/scripts/test_svcomp_adapter.py")
