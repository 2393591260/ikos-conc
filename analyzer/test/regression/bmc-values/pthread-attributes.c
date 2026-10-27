// Real target <pthread.h>: 36/4 on GNU i386, 56/8 on GNU x86_64.
// CASE 10 is a refusal control only: POSIX join of a detached thread is undefined.
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <setjmp.h>
#ifndef CASE
#define CASE 0
#endif
extern int __VERIFIER_nondet_int(void);
extern void unknown_attribute_use(void *);
extern void unknown_unrelated(void);
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int shared;
static void *volatile escaped;
#if CASE == 38
static pthread_attr_t attr;
#endif
static void *worker(void *unused) {
  (void)unused;
  pthread_mutex_lock(&lock);
  shared = 1;
  pthread_mutex_unlock(&lock);
  return 0;
}
#if CASE == 29
int main(void);
__attribute__((noinline)) static int call_main(void) { return main(); }
#endif
int main(void) {
#if CASE == 38
  /* Global storage deliberately refuses. */
#elif CASE == 39
  pthread_attr_t attributes[2];
#define attr attributes[0]
#elif CASE == 40
  struct __attribute__((packed)) { char c; pthread_attr_t member; } object;
#define attr object.member
#elif CASE == 41
  pthread_attr_t *heap_attr = malloc(sizeof(*heap_attr));
#define attr (*heap_attr)
#else
  pthread_attr_t attr;
#endif
  pthread_t thread[2];
#if CASE == 28
  if (__VERIFIER_nondet_int()) return main();
#elif CASE == 29
  if (__VERIFIER_nondet_int()) return call_main();
#elif CASE == 30
  escaped = (void *)&main;
#elif CASE == 31
  *(volatile int *)(uintptr_t)4096 = 1;
#elif CASE == 32
  escaped = __builtin_frame_address(0);
#elif CASE == 33
  asm volatile("" : : : "memory");
#elif CASE == 34
  jmp_buf jump; if (setjmp(jump)) return 0;
#elif CASE == 35
  unknown_unrelated();
#endif
#if CASE == 6 || CASE == 42
  pthread_t independent;
  if (pthread_create(&independent, 0, worker, 0)) return 0;
#endif
#if CASE == 15
  if (pthread_create(&thread[0], &attr, worker, 0)) return 0;
#endif
#if CASE == 18
  for (int n = 0; n < 2; ++n) {
    if (pthread_attr_init(&attr)) return 0;
  }
#elif CASE == 13
  pthread_attr_init(&attr);
#elif CASE == 19
  int stale = __VERIFIER_nondet_int();
  pthread_attr_init(&attr);
  if (stale) return 0;
#else
  if (pthread_attr_init(&attr)) {
#if CASE == 6
    shared = 2; /* INIT_FAILURE_ACCESS: independent NULL-attr worker already exists. */
#endif
    return 0;
  }
#endif
#if CASE == 17
  if (pthread_attr_init(&attr)) return 0;
#endif
#if CASE == 20
  ((volatile unsigned char *)&attr)[0] = 0;
#elif CASE == 23
  unknown_attribute_use(&attr);
#elif CASE == 24
  escaped = &attr;
#elif CASE == 25
  if (pthread_attr_setstacksize(&attr, 65536)) return 0;
#elif CASE == 26
  pthread_attr_t copy = attr;
  unknown_attribute_use(&copy);
#endif
#if CASE != 1
#if CASE == 10 || CASE == 11
  if (pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED)) return 0;
#elif CASE == 12
  if (pthread_attr_setdetachstate(&attr, __VERIFIER_nondet_int())) return 0;
#elif CASE == 14
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
#else
  if (pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE)) {
#if CASE == 42
    shared = 2; /* SETTER_FAILURE_ACCESS: independent worker remains live. */
#endif
    return 0;
  }
#endif
#endif
#if CASE == 16
  if (pthread_attr_destroy(&attr)) return 0;
#endif
#if CASE == 3
  for (int n = 0; n < 2; ++n) {
    if (pthread_create(&thread[n], &attr, worker, 0)) return 0;
  }
  for (int n = 0; n < 2; ++n) if (pthread_join(thread[n], 0)) return 0;
#else
#if CASE == 21
  if (pthread_create(&thread[0], &attr, worker, &attr)) return 0;
#elif CASE == 22
  if (pthread_create((pthread_t *)&attr, &attr, worker, 0)) return 0;
#elif CASE == 4
  pthread_attr_t *alias = (pthread_attr_t *)(void *)&attr;
  if (pthread_create(&thread[0], alias, worker, 0)) return 0;
#elif CASE == 27
  pthread_attr_t *alias = (pthread_attr_t *)((unsigned char *)&attr + 1);
  if (pthread_create(&thread[0], alias, worker, 0)) return 0;
#else
  if (pthread_create(&thread[0], &attr, worker, 0)) return 0;
#endif
#if CASE == 7 || CASE == 9
  int destroy_result = pthread_attr_destroy(&attr);
#if CASE == 7
  shared = 2; /* DESTROY_DOES_NOT_JOIN */
#else
  if (destroy_result) shared = 2; /* DESTROY_FAILURE_ACCESS */
#endif
  if (pthread_join(thread[0], 0)) return 0;
#elif CASE == 11
  shared = 2; /* Legal detached/no-join conflict; certificate must refuse. */
#else
  if (pthread_join(thread[0], 0)) return 0;
#endif
#endif
#if CASE == 6 || CASE == 42
  if (pthread_join(independent, 0)) return 0;
#endif
#if CASE != 5 && CASE != 7 && CASE != 9 && CASE != 16
  if (pthread_attr_destroy(&attr)) return 0;
#endif
#if CASE == 2
  pthread_attr_t other;
  if (pthread_attr_init(&other)) return 0;
  if (pthread_attr_setdetachstate(&other, PTHREAD_CREATE_JOINABLE)) return 0;
  if (pthread_create(&thread[1], &other, worker, 0)) return 0;
  if (pthread_join(thread[1], 0)) return 0;
  if (pthread_attr_destroy(&other)) return 0;
#endif
#if CASE != 11
  shared = 3;
#endif
  return 0;
}
