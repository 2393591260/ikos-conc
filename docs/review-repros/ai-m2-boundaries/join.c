// Distinguish bounded dynamic instances from static create-site identities.
#include <pthread.h>
#ifndef CASE
#define CASE 0
#endif
static int data;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static void *worker(void *arg) {
  pthread_mutex_lock(&lock);
  data = 1;
  pthread_mutex_unlock(&lock);
  return arg;
}
int main(void) {
#if CASE == 0 || CASE == 5
  pthread_t handles[4];
  for (int i = 0; i < 4; ++i) pthread_create(&handles[i], 0, worker, 0);
  for (int i = 0; i < (CASE == 5 ? 3 : 4); ++i) pthread_join(handles[i], 0);
#elif CASE == 2
  pthread_t handles[4];
  pthread_create(&handles[0], 0, worker, 0);
  pthread_create(&handles[1], 0, worker, 0);
  pthread_create(&handles[2], 0, worker, 0);
  pthread_create(&handles[3], 0, worker, 0);
  pthread_join(handles[0], 0);
  pthread_join(handles[1], 0);
  pthread_join(handles[2], 0);
  pthread_join(handles[3], 0);
#else
  pthread_t a, b, c, d;
  pthread_create(&a, 0, worker, 0);
  pthread_create(&b, 0, worker, 0);
  pthread_create(&c, 0, worker, 0);
  pthread_create(&d, 0, worker, 0);
#if CASE == 4
  pthread_create(&a, 0, worker, 0); // original a remains live
#endif
  pthread_join(a, 0);
  pthread_join(b, 0);
  pthread_join(c, 0);
#if CASE != 3
  pthread_join(d, 0);
#endif
#endif
  return data;
}
