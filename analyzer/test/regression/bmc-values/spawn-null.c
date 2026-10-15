#include <pthread.h>
#include <stdint.h>

extern int __VERIFIER_nondet_int(void);
int data[2];
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
#if CASE == 7 && defined(HELPER_GUARD)
void gate(int value) { if (!value) __builtin_abort(); }
#endif

void *worker(void *arg) {
#if CASE == 6
  *(int *)arg = 1;
  return 0;
#else
#ifdef WIDE
  long long index = (intptr_t)arg;
#else
  intptr_t index = (intptr_t)arg;
#endif
#if CASE == 1 || CASE == 2 || CASE == 5
  if (index != 0) return 0;
#elif CASE == 3
  if (arg != 0) return 0;
#elif CASE == 7
#ifdef HELPER_GUARD
  gate(index);
#else
  if (index == 0) return 0;
#endif
#endif
#if CASE == 4
  pthread_mutex_lock(&mutex);
#endif
#if CASE == 3 || CASE == 7
  data[0] = 1;
#else
  data[index] = 1;
#endif
#if CASE == 4
  pthread_mutex_unlock(&mutex);
#endif
  return 0;
#endif
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
#elif CASE == 6
  volatile uintptr_t address = (uintptr_t)&data[1];
  pthread_create(&threads[0], 0, worker, &data[0]);
  pthread_create(&threads[1], 0, worker, (void *)address);
  data[1] = 2;
#elif CASE == 7
  intptr_t value;
  if (__VERIFIER_nondet_int()) value = 0;
  else value = 1;
  pthread_create(&threads[0], 0, worker, (void *)value);
  data[0] = 2;
#else
  for (int i = 0; i < 2; ++i) {
    pthread_create(&threads[i], 0, worker, (void *)(intptr_t)(i / 2));
  }
#endif
  pthread_join(threads[0], 0);
#if CASE != 7
  pthread_join(threads[1], 0);
#endif
  return 0;
}
