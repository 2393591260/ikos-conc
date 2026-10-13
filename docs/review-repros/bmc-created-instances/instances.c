#include <pthread.h>
#include <stdint.h>

#ifndef CASE
#define CASE 0
#endif
#ifndef VARIANT
#define VARIANT 0
#endif

#if VARIANT == 1
#define writer_a renamed_producer
#define writer_b renamed_consumer
#define x first_cell
#define y second_cell
typedef uint64_t value_type;
#else
typedef int value_type;
#endif

extern unsigned __VERIFIER_nondet_uint(void);
extern void __VERIFIER_assume(int);
extern void unknown_effect(void *);

value_type x, y;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

static void *writer_a(void *p) {
#if CASE == 7
  pthread_mutex_lock(&mutex);
#endif
  *(value_type *)p = 1;
#if CASE == 7
  pthread_mutex_unlock(&mutex);
#endif
  return 0;
}

static void *writer_b(void *p) {
#if CASE == 7
  pthread_mutex_lock(&mutex);
#endif
  *(value_type *)p = 2;
#if CASE == 7
  pthread_mutex_unlock(&mutex);
#endif
  return 0;
}

static void *dormant(void *p) {
  unknown_effect(p);
  return 0;
}

int main(void) {
  pthread_t a0, a1, a2, b, unused;
  pthread_t array_handles[3];
#if CASE == 18
  struct { int padding; pthread_t handles[2]; } nested;
  pthread_create(&nested.handles[1], 0, writer_a, &x);
  pthread_create(&a1, 0, writer_a, &y);
  pthread_create(&b, 0, writer_b, &x);
#elif CASE == 16 || CASE == 17
  for (int i = 0; i < 2; ++i) {
    value_type *p = i == 0 ? &x : &y;
    pthread_create(&array_handles[i], 0, writer_a, p);
  }
#if CASE == 16
  pthread_create(&b, 0, writer_b, &y);
#endif
#elif CASE == 11
  for (int i = 0; i < 3; ++i) pthread_create(&array_handles[i], 0, writer_a, &x);
#elif CASE == 12
  pthread_create(&array_handles[3], 0, writer_a, &x);
  pthread_create(&b, 0, writer_b, &x);
#elif CASE == 13
  pthread_create((pthread_t *)((unsigned char *)array_handles + 1), 0, writer_a, &x);
  pthread_create(&b, 0, writer_b, &x);
#elif CASE == 14
  unsigned index = __VERIFIER_nondet_uint();
  __VERIFIER_assume(index < 3);
  pthread_create(&array_handles[index], 0, writer_a, &x);
  pthread_create(&b, 0, writer_b, &x);
#elif CASE == 15
  pthread_create(&array_handles[3], 0, dormant, &x);
  pthread_create(&a0, 0, writer_a, &x);
  pthread_create(&b, 0, writer_b, &x);
#elif CASE == 5
  unsigned selector = __VERIFIER_nondet_uint();
  if (selector == 0) pthread_create(&a0, 0, writer_a, &x);
  if (selector != 0) pthread_create(&a1, 0, writer_a, &x);
#elif CASE == 10
  unsigned selector = __VERIFIER_nondet_uint();
  __VERIFIER_assume(selector == 0);
  pthread_create(&a0, 0, writer_a, &x);
  if (selector != 0) pthread_create(&a1, 0, writer_a, &x);
#else
#if CASE == 4
  pthread_create(&unused, 0, dormant, &x);
#endif
  pthread_create(&a0, 0, writer_a, &x);
#if CASE == 1
  pthread_create(&b, 0, writer_b, &y);
#elif CASE == 2
  pthread_create(&a1, 0, writer_a, &x);
#elif CASE == 3
  pthread_create(&a1, 0, writer_a, &y);
#elif CASE == 6
  /* The single instance cannot race with another invocation of itself. */
#else
  pthread_create(&a1, 0, writer_a, &y);
#if CASE == 8
  pthread_create(&a2, 0, writer_a, &x);
#elif CASE == 9
  pthread_create(&a2, 0, writer_a, &y);
#else
  pthread_create(&b, 0, writer_b, &y);
#endif
#endif
#endif
  return 0;
}
