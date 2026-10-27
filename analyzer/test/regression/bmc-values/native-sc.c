#include <pthread.h>
#include <stdint.h>
#ifndef CASE
#define CASE 0
#endif
#if defined(TINY)
typedef unsigned char datum;
#elif defined(SHORT)
typedef unsigned short datum;
#elif defined(WIDE)
typedef unsigned long long datum;
#else
typedef unsigned int datum;
#endif
static datum cell __attribute__((aligned(8)));
static datum independent_cell __attribute__((aligned(8)));
static volatile int payload;
#if CASE == 15
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
#elif CASE == 16
static pthread_barrier_t barrier;
#endif
static void need(int x) { if (!x) __builtin_abort(); }
static datum compare_old(datum expected, datum desired) {
  __atomic_compare_exchange_n(&cell, &expected, desired, 0, 5, 5);
  return expected;
}
static void *first(void *arg) {
  (void)arg;
#if CASE == 0 || CASE == 12
  datum expected = 0;
  need(__atomic_compare_exchange_n(&cell, &expected, 1, 0, 5, 5));
  need(expected == 0);
  payload = 1;
#elif CASE == 1 || CASE == 3
  payload = 1;
  __atomic_store_n(&cell, 1, 5);
#elif CASE == 2
  payload = 1;
  datum expected = 0;
  need(!__atomic_compare_exchange_n(&cell, &expected, 2, 0, 5, 5));
  need(expected == 1);
#elif CASE == 4
  datum expected = 0;
  need(!__atomic_compare_exchange_n(&cell, &expected, 1, 1, 5, 5));
  need(expected == 0); /* permitted spurious weak failure */
  payload = 1;
#elif CASE == 5
  need(__atomic_fetch_add(&cell, 1, 5) == (datum)-1);
  payload = 1;
#elif CASE == 6
  __asm__ __volatile__("" ::: "memory");
  (void)__atomic_load_n(&independent_cell, 5);
  payload = 1;
#elif CASE == 7
  (void)__atomic_load_n(&cell, 2); /* unsupported weaker order */
  payload = 1;
#elif CASE == 8
  *(volatile datum *)&cell = 0; /* broken mixed-access premise */
  payload = 1;
#elif CASE == 9
  __atomic_thread_fence(5); /* not in this subset */
  payload = 1;
#elif CASE == 10
  need(compare_old(0, 1) == 0);
  payload = 1;
#elif CASE == 11
  datum expected = 0;
  need(!__atomic_compare_exchange_n(&cell, &expected, 1, 0, 5, 5));
  need(expected == 0); /* impossible strong equal failure */
  payload = 1;
#elif CASE == 13
  (void)__atomic_exchange_n(&cell, 1, 5);
  (void)__atomic_fetch_sub(&cell, 1, 5);
  (void)__atomic_fetch_and(&cell, 3, 5);
  (void)__atomic_fetch_or(&cell, 4, 5);
  (void)__atomic_fetch_xor(&cell, 1, 5);
  payload = 1;
#elif CASE == 15
  pthread_mutex_lock(&mutex);
  pthread_cond_wait(&condition, &mutex); /* finite spurious wake */
  pthread_mutex_unlock(&mutex);
  (void)__atomic_load_n(&cell, 5);
  payload = 1;
#elif CASE == 16
  pthread_barrier_wait(&barrier);
  (void)__atomic_load_n(&cell, 5);
  payload = 1;
#elif CASE == 14
  __asm__ __volatile__("mfence" ::: "memory"); /* machine asm not inferred */
  payload = 1;
#endif
  return 0;
}
static void *second(void *arg) {
  (void)arg;
#if CASE == 0 || CASE == 10 || CASE == 12
  datum expected = 0;
  int ok = __atomic_compare_exchange_n(&cell, &expected, 2, 0, 5, 5);
  need(!ok && expected == 1);
#elif CASE == 1
  need(__atomic_load_n(&cell, 5) == 1);
#elif CASE == 2
  need(__atomic_load_n(&cell, 5) == 1);
#elif CASE == 3
  datum expected = 0;
  need(!__atomic_compare_exchange_n(&cell, &expected, 2, 0, 5, 5));
  need(expected == 1);
#elif CASE == 5
  need(__atomic_load_n(&cell, 5) == 0);
#elif CASE == 6
  __asm__ __volatile__("" ::: "memory");
  (void)__atomic_load_n(&cell, 5);
#elif CASE == 8
  (void)__atomic_load_n(&cell, 5);
#endif
#if CASE == 15
  (void)__atomic_load_n(&independent_cell, 5);
#elif CASE == 16
  pthread_barrier_wait(&barrier);
  (void)__atomic_load_n(&independent_cell, 5);
#endif
  (void)payload;
  return 0;
}
int main(void) {
  pthread_t a, b;
#if CASE == 16
  pthread_barrier_init(&barrier, 0, 2);
#endif
#if CASE == 2
  cell = 1;
#elif CASE == 5
  cell = (datum)-1;
#endif
  pthread_create(&a, 0, first, 0);
#if CASE == 16
  pthread_barrier_wait(&barrier);
  (void)__atomic_load_n(&independent_cell, 5);
  (void)payload;
#else
  pthread_create(&b, 0, second, 0);
#endif
  return 0;
}
