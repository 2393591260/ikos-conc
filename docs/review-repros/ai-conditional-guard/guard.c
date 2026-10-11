#include <pthread.h>
extern unsigned __VERIFIER_nondet_uint(void);
extern unsigned long long __VERIFIER_nondet_ulonglong(void);
#ifndef CASE
#define CASE 0
#endif
static int payload;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static void *worker(void *arg) {
  (void)arg;
  pthread_mutex_lock(&mutex);
  payload = 1;
  pthread_mutex_unlock(&mutex);
  return 0;
}
__attribute__((noinline)) static void conditional(unsigned guard, int acquire) {
  if (acquire) {
    if (guard) pthread_mutex_lock(&mutex);
  }
  if (guard) payload = 2;
}
int main(void) {
  pthread_t thread;
  unsigned index = __VERIFIER_nondet_uint();
  pthread_create(&thread, 0, worker, 0);
#if CASE == 0
  if ((unsigned char)index) pthread_mutex_lock(&mutex);
  if (index) payload = 2;
  if ((unsigned char)index) pthread_mutex_unlock(&mutex);
#elif CASE == 1
  if (index) pthread_mutex_lock(&mutex);
  if (index) payload = 2;
  if (index) pthread_mutex_unlock(&mutex);
#elif CASE == 2
  unsigned char narrow = index;
  if (narrow) pthread_mutex_lock(&mutex);
  if (narrow) payload = 2;
  if (narrow) pthread_mutex_unlock(&mutex);
#elif CASE == 3
  unsigned char narrow = index;
  if ((unsigned)narrow) pthread_mutex_lock(&mutex);
  if (narrow) payload = 2;
  if ((unsigned)narrow) pthread_mutex_unlock(&mutex);
#elif CASE == 4
  if ((signed char)index) pthread_mutex_lock(&mutex);
  if (index) payload = 2;
  if ((signed char)index) pthread_mutex_unlock(&mutex);
#elif CASE == 5
  if ((int)index) pthread_mutex_lock(&mutex);
  if (index) payload = 2;
  if ((int)index) pthread_mutex_unlock(&mutex);
#elif CASE == 6
  if ((int)index > 0) pthread_mutex_lock(&mutex);
  if (index) payload = 2;
  if ((int)index > 0) pthread_mutex_unlock(&mutex);
#elif CASE == 7 || CASE == 8
  conditional(index, 1);
  conditional(CASE == 7 ? 1 : index, 0);
  if (index) pthread_mutex_unlock(&mutex);
#elif CASE == 9
  unsigned held = index;
  for (unsigned step = 0; step < 2; ++step) {
    if (step == 0) {
      if (index) pthread_mutex_lock(&mutex);
    } else {
      index = 1;
      if (index) payload = 2;
    }
  }
  if (held) pthread_mutex_unlock(&mutex);
#elif CASE == 10
  unsigned long long wide = __VERIFIER_nondet_ulonglong();
  if ((unsigned)wide) pthread_mutex_lock(&mutex);
  if (wide) payload = 2;
  if ((unsigned)wide) pthread_mutex_unlock(&mutex);
#elif CASE == 11
  if (index == 1) pthread_mutex_lock(&mutex);
  if (index) payload = 2;
  if (index == 1) pthread_mutex_unlock(&mutex);
#endif
  pthread_join(thread, 0);
  return 0;
}
