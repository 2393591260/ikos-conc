// SPDX-License-Identifier: MIT
// Cross-component controls. Macro names select test inputs only.
#include <pthread.h>
#include <stdlib.h>
#ifndef CASE
#define CASE 0
#endif
#ifndef A_COUNT
#define A_COUNT 2
#endif
#ifndef B_COUNT
#define B_COUNT 3
#endif
#ifdef WIDE
typedef long long datum;
#else
typedef int datum;
#endif
#if CASE == 8
#define GENERATIONS 3
#else
#define GENERATIONS 2
#endif
extern void __VERIFIER_assume(int);
static volatile int ready;
static volatile datum cells[4];
static volatile datum protected_cell;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

__attribute__((noinline)) static int write_cell(volatile datum *p, int value) {
  *p = 0;
  return value + 3;
}
__attribute__((noinline)) static datum read_cell(volatile datum *p) { return *p; }
__attribute__((noinline)) static int __VERIFIER_atomic_write_cell(
    volatile datum *p, int value) {
  *p = 0;
  return value + 3;
}

static void *provider(void *unused) {
  (void)unused;
#if CASE == 1
  ready = 0;
#else
  ready = 1;
#endif
#if CASE == 2
  __VERIFIER_assume(0);
#elif CASE == 5
  ready = 0;
#endif
  return 0;
}

static void *worker_a(void *argument) {
#if (CASE >= 6 && CASE <= 12)
  volatile datum *base = argument;
#else
  (void)argument;
  volatile datum *base = cells;
#endif
#if CASE == 13
  pthread_mutex_lock(&lock);
  protected_cell = 0;
  pthread_mutex_unlock(&lock);
#endif
  for (int i = 0; i < A_COUNT; ++i) {
#if (CASE >= 6 && CASE <= 12)
    volatile datum *p = base + (i + 1 == A_COUNT ? 1 : 2);
#else
    volatile datum *p = base + (i + 1 == A_COUNT ? 0 : 1);
#endif
#if CASE == 3
    pthread_mutex_lock(&lock);
#endif
#if CASE == 8 || CASE == 15
    if (write_cell(p, i + 1) != i + 4) abort();
#elif CASE == 16 || CASE == 18
    if (__VERIFIER_atomic_write_cell(p, i + 1) != i + 4) abort();
#else
    *p = 0;
#endif
#if CASE == 3
    pthread_mutex_unlock(&lock);
#endif
  }
  return 0;
}

static void *worker_b(void *argument) {
#if (CASE >= 6 && CASE <= 12)
  volatile datum *base = argument;
#else
  (void)argument;
  volatile datum *base = cells;
#endif
#if CASE == 13
  pthread_mutex_lock(&lock);
  (void)protected_cell;
  pthread_mutex_unlock(&lock);
#endif
  for (int i = 0; i < B_COUNT; ++i) {
#if (CASE >= 6 && CASE <= 12)
    volatile datum *p = base + (i + 1 == B_COUNT ? 1 : 3);
#else
    volatile datum *p = base + (i + 1 == B_COUNT ? 0 : 2);
#endif
#if CASE == 3
    pthread_mutex_lock(&lock);
#endif
#if CASE == 8 || CASE == 15
    (void)read_cell(p);
#else
    (void)*p;
#endif
#if CASE == 3
    pthread_mutex_unlock(&lock);
#endif
  }
#if CASE == 18
  __VERIFIER_assume(0); // Atomic-target permission needs this root's real Return.
#endif
  return 0;
}

#if CASE == 17 || CASE == 19
static void *worker_same(void *argument) {
  volatile datum *private_cell = argument;
  for (int i = 0; i < 3; ++i) {
    volatile datum *target = i == 2 ? &cells[0] : private_cell;
#if CASE == 19
    // All retained writes remain disjoint between the actual two instances.
    if (i == 2 && private_cell == &cells[2]) target = &cells[3];
#endif
    *target = 0;
  }
  return 0;
}
#endif

#if CASE == 14 || CASE == 20
static void *worker_window(void *argument) {
  *(datum *)argument = 0; // Per-instance cell: window exclusion is applicable.
#if CASE == 14
  cells[0] = 0;          // Shared cell: still needs a genuine BMC execution.
#endif
  return 0;
}
int main(void) {
  datum *data = malloc(2 * sizeof(datum));
  if (!data) abort();
  pthread_t ids[2];
  for (int instance = 0; instance < 2; ++instance) {
#if CASE == 20
    // Actual duplicate argument destroys the per-instance stride premise.
    pthread_create(&ids[instance], 0, worker_window, &data[instance / 2]);
#else
    pthread_create(&ids[instance], 0, worker_window, &data[instance]);
#endif
  }
  pthread_join(ids[0], 0);
  pthread_join(ids[1], 0);
  return 0;
}
#else
int main(void) {
  volatile datum *first = 0, *selected = 0;
#if CASE >= 6 && CASE <= 12
  for (unsigned generation = 0; generation < GENERATIONS; ++generation) {
    datum *p = malloc((generation + 4) * sizeof(datum));
    if (!p) abort();
    // Equal bytes at equal offsets make wrong-generation rf a semantic
    // identity error even when the encoded read result remains unchanged.
    p[1] = 0;
    p[2] = 0;
    p[3] = 0;
    if (generation == 0) {
      first = p;
#if CASE == 7 || CASE == 9
      free(p);
#endif
    }
    selected = p;
  }
#endif
  pthread_t p, a, b;
#if CASE <= 5 || CASE == 12 || CASE >= 15
  pthread_create(&p, 0, provider, 0);
#if CASE == 4
  pthread_join(p + 1, 0);
#else
  pthread_join(p, 0);
#endif
  int observed = ready;
  if (observed != 1) return 0;
#endif
#if CASE == 17 || CASE == 19
  pthread_t ids[2];
  for (int instance = 0; instance < 2; ++instance) {
    pthread_create(&ids[instance], 0, worker_same, (void *)&cells[instance + 1]);
  }
  pthread_join(ids[0], 0);
  pthread_join(ids[1], 0);
  return 0;
#elif CASE >= 6 && CASE <= 12
  pthread_create(&a, 0, worker_a, (void *)selected);
#if CASE == 11
  free((void *)selected); // Outside the initial-main heap-generation profile.
#endif
#if CASE == 9 || CASE == 10
  pthread_create(&b, 0, worker_b, (void *)first);
#else
  pthread_create(&b, 0, worker_b, (void *)selected);
#endif
#else
  pthread_create(&a, 0, worker_a, 0);
  pthread_create(&b, 0, worker_b, 0);
#endif
#if CASE != 17 && CASE != 19
  pthread_join(a, 0);
  pthread_join(b, 0);
  return 0;
#endif
}
#endif
