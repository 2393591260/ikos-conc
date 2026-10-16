#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#ifndef CASE
#define CASE 0
#endif
#ifdef WIDE
typedef long long datum;
#else
typedef int datum;
#endif
extern int __VERIFIER_nondet_int(void);
extern void opaque(void *);
static __thread datum *local_slot;
static datum *shared;
static datum **escaped_slot;
static void *saved;
static volatile int input_count;
static int race_value;
__attribute__((noinline)) static datum *identity(datum *p) { return p; }
__attribute__((noinline)) static void check(int ok) { if (!ok) abort(); }
__attribute__((noinline)) static void mutate_slot(void) { local_slot = shared; }
static void *worker(void *arg) {
  (void)arg;
  int n = __VERIFIER_nondet_int();
#if CASE == 25
  n = input_count;
#endif
  check(n >= 0);
#if CASE == 5
  for (int round = 0; round < 2; ++round) {
#endif
#if CASE == 19
  datum *p = malloc((size_t)n * sizeof(datum));
#elif CASE == 26
  datum *p = calloc((size_t)n, 0);
#elif CASE == 12 || CASE == 27
  datum *p = calloc(4, sizeof(datum));
#else
  datum *p = calloc((size_t)n, sizeof(datum));
#endif
#if CASE != 1
  local_slot = p;
#endif
#if CASE == 2
  shared = p;
#elif CASE == 3
  escaped_slot = &local_slot;
#elif CASE == 4
  mutate_slot();
#elif CASE == 6
  free(p);
#elif CASE == 7
  saved = (void *)(uintptr_t)p;
#elif CASE == 8
  opaque(p);
#elif CASE == 20
  p = identity(p);
#elif CASE == 21
  local_slot = 0;
#elif CASE == 23
  *(void **)p = shared;
#elif CASE == 17
  memcpy(&saved, &p, sizeof(p));
#endif
#if CASE != 1
  p = local_slot;
#endif
#if CASE == 27
  if (p) p[4] = 1;
#elif CASE == 12
  if (p) { p[2] = 1; }
#elif CASE == 13
  if (n > 0) p[-1] = 1;
#else
  for (int i = 0; i < n; ++i) {
    volatile datum x = p[i]; (void)x;
  }
#if CASE == 9
  for (int i = 0; i <= n; ++i) p[i] = 1;
#elif CASE == 10
  for (int i = 0; i < n + 1; ++i) p[i] = 1;
#elif CASE == 11
  for (int i = 0; i < n; ++i) ((long long *)p)[i] = 1;
#elif CASE == 14
  for (int i = 1; i < n; ++i) p[i] = 1;
#elif CASE == 15
  for (int i = 0; i < n; i += 2) p[i] = 1;
#elif CASE == 22
  for (int i = 0; i < n; ++i) local_slot[i] = 1;
#else
  for (int i = 0; i < n; ++i) p[i] = 1;
#endif
#endif
#if CASE == 24
  race_value++;
#endif
#if CASE != 6
  free(p);
#endif
#if CASE == 18
  if (local_slot) saved = local_slot;
#endif
#if CASE == 5
  }
#endif
  return 0;
}
#if CASE == 16
static void *other(void *p) { return worker(p); }
#endif
int main(void) {
  pthread_t ids[2];
#if CASE == 16
  worker(0);
#endif
  for (int i = 0; i < 2; ++i) pthread_create(&ids[i], 0, worker, 0);
  for (int i = 0; i < 2; ++i) pthread_join(ids[i], 0);
#if CASE == 16
  other(0);
#endif
  return 0;
}
