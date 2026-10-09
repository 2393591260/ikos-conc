#include <pthread.h>

#ifndef CASE
#define CASE 0
#endif

extern int __VERIFIER_nondet_int(void);
extern void opaque(void);
volatile int selector;
int data;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

static void* guarded(void* unused) {
  opaque(); // This candidate lacks a call summary; later candidates remain useful.
  if (selector)
    data = 1;
  return 0;
}

static void store_data(void) {
#if CASE == 2
  pthread_mutex_lock(&mutex);
#endif
  data = 2;
#if CASE == 2
  pthread_mutex_unlock(&mutex);
#endif
}

static void* plain(void* unused) {
#if CASE == 1
  if (selector)
    store_data();
#elif CASE == 3
  // Backedges used to pop blocks owned by the caller's DFS stack.
  volatile int local = 0;
  for (int i = 0; i < 2; ++i) {
    for (int j = 0; j < 2; ++j)
      local = 1;
  }
  store_data();
#else
  store_data();
#endif
  return 0;
}

int main(void) {
  pthread_t a, b;
  selector = 1;
#if CASE != 2 && CASE != 3
  pthread_create(&a, 0, guarded, 0);
#endif
  pthread_create(&b, 0, plain, 0);
#if CASE == 2
  pthread_mutex_lock(&mutex);
#endif
  data = 3;
#if CASE == 2
  pthread_mutex_unlock(&mutex);
#endif
#if CASE != 2 && CASE != 3
  pthread_join(a, 0);
#endif
  pthread_join(b, 0);
  return 0;
}
