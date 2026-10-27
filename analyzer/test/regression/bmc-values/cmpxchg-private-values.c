#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#ifndef CASE
#define CASE 0
#endif
#ifndef CELL64
#define CELL64 0
#endif
#if CELL64
typedef uint64_t cell_t;
#else
typedef unsigned cell_t;
#endif
int payload;
static void *worker(void *arg) {
  (void)arg;
  _Atomic(cell_t) cell = 1;
  cell_t expected = (CASE == 0 || CASE == 2 || CASE == 5) ? 0 : 1;
  _Bool ok;
#if CASE >= 4
  ok = atomic_compare_exchange_weak_explicit(&cell, &expected, 2,
                                             memory_order_acq_rel,
                                             memory_order_relaxed);
#else
  ok = atomic_compare_exchange_strong_explicit(&cell, &expected, 2,
                                               memory_order_seq_cst,
                                               memory_order_seq_cst);
#endif
  cell_t after = atomic_load_explicit(&cell, memory_order_relaxed);
#if CASE == 0
  if (!ok && expected == 1 && after == 1) ++payload;
#elif CASE == 1
  if (ok && expected == 1 && after == 2) ++payload;
#elif CASE == 2
  if (after == 2) ++payload;
#elif CASE == 3
  if (!ok) ++payload;
#elif CASE == 4
  if (!ok && expected == 1 && after == 1) ++payload;
#elif CASE == 5
  if (ok || after == 2) ++payload;
#elif CASE == 6
  if (ok && after == 2) ++payload;
#endif
  return 0;
}
int main(void) {
  pthread_t a, b;
  pthread_create(&a, 0, worker, 0);
  pthread_create(&b, 0, worker, 0);
  return 0;
}
