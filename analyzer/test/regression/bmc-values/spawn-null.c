#include <pthread.h>
#include <stdint.h>

extern int __VERIFIER_nondet_int(void);
int data[2];
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

void *worker(void *arg) {
#ifdef WIDE
  long long index = (intptr_t)arg;
#else
  intptr_t index = (intptr_t)arg;
#endif
#if CASE == 1 || CASE == 2 || CASE == 5
  if (index != 0) return 0;
#elif CASE == 3
  if (arg != 0) return 0;
#endif
#if CASE == 4
  pthread_mutex_lock(&mutex);
#endif
#if CASE == 3
  data[0] = 1;
#else
  data[index] = 1;
#endif
#if CASE == 4
  pthread_mutex_unlock(&mutex);
#endif
  return 0;
}

int main(void) {
  pthread_t threads[2];
#if CASE == 1
  pthread_create(&threads[0], 0, worker, 0);
  pthread_create(&threads[1], 0, worker, (void *)(intptr_t)1);
  data[0] = 2;
#elif CASE == 2
  void *arg = __VERIFIER_nondet_int() ? 0 : (void *)(intptr_t)1;
  pthread_create(&threads[0], 0, worker, arg);
  pthread_create(&threads[1], 0, worker, (void *)(intptr_t)1);
  data[0] = 2;
#elif CASE == 3
  pthread_create(&threads[0], 0, worker, 0);
  pthread_create(&threads[1], 0, worker, &data[1]);
  data[0] = 2;
#elif CASE == 5
  pthread_create(&threads[0], 0, worker, (void *)(intptr_t)1);
  pthread_create(&threads[1], 0, worker, (void *)(intptr_t)1);
#else
  for (int i = 0; i < 2; ++i) {
    pthread_create(&threads[i], 0, worker, (void *)(intptr_t)(i / 2));
  }
#endif
  pthread_join(threads[0], 0);
  pthread_join(threads[1], 0);
  return 0;
}
