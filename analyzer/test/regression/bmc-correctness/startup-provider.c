/* A timed main prefix plus one real provider and two racing targets. */
#include <pthread.h>
#ifndef CASE
#define CASE 0
#endif
extern void __VERIFIER_assume(int);
extern void __VERIFIER_atomic_begin(void);
extern void __VERIFIER_atomic_end(void);
static volatile int ready;
static volatile int payload;
static pthread_mutex_t communication = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t target_lock = PTHREAD_MUTEX_INITIALIZER;

__attribute__((noinline)) static void publish(int value) { ready = value; }

static void* provider(void* unused) {
  (void)unused;
#if CASE == 7 || CASE == 8
  pthread_mutex_lock(&communication);
#endif
#if CASE == 1
  ready = 0;
#elif CASE == 9
  publish(1);
#else
  ready = 1;
#endif
#if CASE == 3
  __VERIFIER_assume(0);
#elif CASE == 5
  ready = 0;
#endif
#if CASE == 7 || CASE == 8
  pthread_mutex_unlock(&communication);
#endif
  return 0;
}

static void* worker_a(void* unused) {
  (void)unused;
#if CASE == 13
  pthread_mutex_lock(&communication);
  ready = 1;
  pthread_mutex_unlock(&communication);
#endif
#if CASE == 6
  pthread_mutex_lock(&target_lock);
#endif
  payload = 1;
#if CASE == 6
  pthread_mutex_unlock(&target_lock);
#elif CASE == 12
  __VERIFIER_assume(0);
#endif
  return 0;
}

static void* worker_b(void* unused) {
  (void)unused;
#if CASE == 6
  pthread_mutex_lock(&target_lock);
#elif CASE == 10 || CASE == 12
  __VERIFIER_atomic_begin();
#endif
  int value = payload;
  (void)value;
#if CASE == 6
  pthread_mutex_unlock(&target_lock);
#elif CASE == 10 || CASE == 12
  __VERIFIER_atomic_end();
#endif
  return 0;
}

int main(void) {
  pthread_t p, a, b;
#if CASE == 13
  // No separate provider exists. A's publication must run before this read;
  // its later payload write remains outside the communication critical section.
  pthread_create(&a, 0, worker_a, 0);
  pthread_mutex_lock(&communication);
  int observed = ready;
  pthread_mutex_unlock(&communication);
  if (observed == 1) {
    pthread_create(&b, 0, worker_b, 0);
    pthread_join(a, 0);
    pthread_join(b, 0);
  }
#else
#if CASE == 8
  pthread_mutex_lock(&communication);
#endif
  pthread_create(&p, 0, provider, 0);
#if CASE == 7
  pthread_mutex_lock(&communication);
#elif CASE == 2
  pthread_join(p + 1, 0);
#elif CASE == 11
  pthread_t copy = p;
  pthread_join(copy, 0);
#else
  pthread_join(p, 0);
#endif
#if CASE == 4
  pthread_join(p, 0);
#endif
  int observed = ready;
#if CASE == 7 || CASE == 8
  pthread_mutex_unlock(&communication);
#endif
  if (observed == 1) {
    pthread_create(&a, 0, worker_a, 0);
    pthread_create(&b, 0, worker_b, 0);
    pthread_join(a, 0);
    pthread_join(b, 0);
  }
#endif
  return 0;
}
