/* Common boilerplate for boundary tests (mirrors clean-sv-benchmarks style). */
#ifndef _BOUNDARY_BOILERPLATE_H_
#define _BOUNDARY_BOILERPLATE_H_
extern int __VERIFIER_nondet_int(void);
extern void __VERIFIER_atomic_begin(void);
extern void __VERIFIER_atomic_end(void);
extern void abort(void);
static void reach_error(void) { __builtin_abort(); }
#include <stdio.h>
#include <pthread.h>
#include <assert.h>
#endif
