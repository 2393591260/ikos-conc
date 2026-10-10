#include <pthread.h>
#include <stdatomic.h>

#ifndef CASE
#define CASE 0
#endif
#if CASE == 1
#define set_flag z_set
#define reset_flag a_reset
#else
#define set_flag a_set
#define reset_flag z_reset
#endif

static _Atomic int flag;
static int value;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
extern int __VERIFIER_nondet_int(void);

static void *set_flag(void *unused) {
#if CASE == 2
  if (__VERIFIER_nondet_int()) atomic_store(&flag, 1);
  else atomic_store(&flag, 0);
#else
  atomic_store(&flag, 1);
#endif
  return 0;
}

static void *reader(void *unused) {
  if (atomic_load(&flag) == 1) {
#if CASE == 3
    pthread_mutex_lock(&mutex);
#endif
    value = 1;
#if CASE == 3
    pthread_mutex_unlock(&mutex);
#endif
  }
  return 0;
}

static void *reset_flag(void *unused) {
  atomic_store(&flag, 0);
#if CASE == 3
  pthread_mutex_lock(&mutex);
#endif
  value = 2;
#if CASE == 3
  pthread_mutex_unlock(&mutex);
#endif
  return 0;
}

int main(void) {
  pthread_t a, b, c;
  pthread_create(&a, 0, set_flag, 0);
  pthread_create(&b, 0, reader, 0);
  pthread_create(&c, 0, reset_flag, 0);
  pthread_join(a, 0);
  pthread_join(b, 0);
  pthread_join(c, 0);
  return 0;
}
