#include <pthread.h>
#include <stdlib.h>
extern int __VERIFIER_nondet_int(void);
#ifndef CASE
#define CASE 0
#endif
int payload, array[2];
pthread_mutex_t guard = PTHREAD_MUTEX_INITIALIZER;
void* worker(void* ignored) {
#if CASE == 3
  pthread_mutex_lock(&guard);
#endif
#if CASE == 6 || CASE == 7
  array[1] = 42;
#else
  payload = 42;
#endif
#if CASE == 3
  pthread_mutex_unlock(&guard);
#endif
  return NULL;
}
void spawn(pthread_t* id) {pthread_create(id, NULL, worker, NULL);}
int main(void) {
  pthread_t ids[10];
#if CASE == 4
  if (__VERIFIER_nondet_int()) pthread_create(&ids[0], NULL, worker, NULL);
  else pthread_create(&ids[1], NULL, worker, NULL);
#elif CASE == 5
  spawn(&ids[0]); spawn(&ids[1]);
#elif CASE == 6 || CASE == 7
  int index = __VERIFIER_nondet_int();
  if (index != 0) return 0;
  int* saved = &array[index];
  ++index;
  pthread_create(&ids[0], NULL, worker, NULL);
#if CASE == 6
  *saved = 1; // array[0] remains distinct after changing index.
#else
  array[index] = 1;
#endif
#else
  for (int i = 0; i <
#if CASE == 2
       1
#else
       10
#endif
       ; ++i) {
    pthread_create(&ids[i], NULL, worker, NULL);
#if CASE == 1
    pthread_join(ids[i], NULL);
#endif
  }
#endif
  return 0;
}
