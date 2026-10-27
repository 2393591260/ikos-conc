#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdatomic.h>
#include <semaphore.h>
#ifndef CASE
#define CASE 0
#endif
#ifdef WIDE
typedef long long datum;
#else
typedef int datum;
#endif
extern int __VERIFIER_nondet_int(void);
extern void __VERIFIER_atomic_begin(void);
extern void __VERIFIER_atomic_end(void);
static void require(int value) { if (!value) __builtin_abort(); }
volatile datum payload;
volatile int left, right, flag, relay_flag;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_rwlock_t rwlock = PTHREAD_RWLOCK_INITIALIZER;
pthread_once_t once = PTHREAD_ONCE_INIT;
pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
pthread_barrier_t barrier;
sem_t semaphore;
_Atomic int native_flag;
static void init(void) { relay_flag = 1; }

static void __VERIFIER_atomic_write(void) {
  left = 1;
#if CASE == 6
  require(flag == 1);
#endif
  payload = 7;
  right = 1;
#if CASE == 5 || CASE == 14
  flag = 1;
#endif
}
static void expanded_writer(void) {
#if CASE == 1
  __VERIFIER_atomic_begin();
  left = 1; payload = 7; right = 1;
  __VERIFIER_atomic_end();
#else
  __VERIFIER_atomic_write();
#endif
}
static void *writer(void *unused) {
  (void)unused;
#if CASE == 10
  pthread_mutex_lock(&mutex);
#elif CASE == 11
  puts("marker");
#elif CASE == 12
  atomic_store_explicit(&native_flag, 1, memory_order_relaxed);
#elif CASE == 22
  pthread_rwlock_wrlock(&rwlock);
#elif CASE == 23
  pthread_cond_signal(&condition);
#elif CASE == 25
  pthread_barrier_wait(&barrier);
#endif
  expanded_writer();
#if CASE == 7
  require(0);
#elif CASE == 13
  for (;;) { }
#elif CASE == 10
  pthread_mutex_unlock(&mutex);
#elif CASE == 21
  sem_post(&semaphore);
#elif CASE == 22
  pthread_rwlock_unlock(&rwlock);
#elif CASE == 19
  __VERIFIER_atomic_begin(); relay_flag = 1; __VERIFIER_atomic_end();
#endif
  return 0;
}
static void *reader(void *unused) {
  (void)unused;
#if CASE == 5 || CASE == 14
  __VERIFIER_atomic_begin(); require(flag == 1); __VERIFIER_atomic_end();
#elif CASE == 10
  pthread_mutex_lock(&mutex);
#elif CASE == 21
  sem_wait(&semaphore);
#elif CASE == 22
  pthread_rwlock_rdlock(&rwlock);
#elif CASE == 24
  pthread_once(&once, init);
#endif
#if CASE == 4
  __VERIFIER_atomic_begin();
#endif
  (void)payload;
#if CASE == 4
  __VERIFIER_atomic_end();
#elif CASE == 6
  __VERIFIER_atomic_begin(); flag = 1; __VERIFIER_atomic_end();
#elif CASE == 8
  require(0);
#elif CASE == 10
  pthread_mutex_unlock(&mutex);
#elif CASE == 22
  pthread_rwlock_unlock(&rwlock);
#elif CASE == 19
  __VERIFIER_atomic_begin(); relay_flag = 2; __VERIFIER_atomic_end();
#endif
  return 0;
}
static void *either(void *unused) {
  if (__VERIFIER_nondet_int()) writer(unused); else reader(unused);
  return 0;
}
static void *relay(void *unused) {
  (void)unused;
  __VERIFIER_atomic_begin(); require(flag == 1); relay_flag = 1;
  __VERIFIER_atomic_end();
  return 0;
}
int main(void) {
  pthread_t a, b;
#if CASE == 21
  sem_init(&semaphore, 0, 0);
#elif CASE == 25
  pthread_barrier_init(&barrier, 0, 1);
#endif
#if CASE == 17
  expanded_writer();
  pthread_create(&a, 0, reader, 0);
  pthread_join(a, 0);
#elif CASE == 18
  for (int i = 0; i < 2; ++i) pthread_create(&a, 0, either, 0);
#elif CASE == 14
  pthread_create(&a, 0, writer, 0);
  pthread_create(&b, 0, relay, 0);
  pthread_join(b, 0);
  (void)payload;
  pthread_join(a, 0);
#elif CASE == 15 || CASE == 16
  pthread_create(&a, 0, writer, 0);
#if CASE == 16
  pthread_join(a, 0);
#endif
  (void)payload;
#if CASE == 15
  pthread_join(a, 0);
#endif
#else
  pthread_create(&a, 0, writer, 0);
  pthread_create(&b, 0, reader, 0);
#endif
  return 0;
}
