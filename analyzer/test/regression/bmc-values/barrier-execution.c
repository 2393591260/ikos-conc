#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#ifndef CASE
#define CASE 0
#endif
pthread_barrier_t gate;
#if CASE == 1 || CASE == 3
pthread_barrier_t other;
#endif
#if CASE == 10
long long value;
#else
int value;
#endif
pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
void *worker(void *arg) {
#if CASE == 3
  pthread_barrier_wait(&other);
#else
  pthread_barrier_wait(&gate);
#endif
#if CASE == 7
  pthread_barrier_wait(&gate);
#endif
#if CASE == 4
  pthread_mutex_lock(&lock);
#endif
  value=1;
#if CASE == 4
  pthread_mutex_unlock(&lock);
#endif
#if CASE == 1
  pthread_barrier_wait(&other);
#endif
  return 0;
}
int main(int argc, char **argv) {
#if CASE == 5
  pthread_barrier_init(&gate,0,3);
#elif CASE != 6
  pthread_barrier_init(&gate,0,2);
#endif
#if CASE == 1 || CASE == 3
  pthread_barrier_init(&other,0,2);
#endif
#if CASE == 11
  pthread_barrier_init(&gate,0,2); /* repeated init outside one-shot seal */
#elif CASE == 12
  ((unsigned char *)&gate)[0]=0; /* raw opaque storage use */
#endif
  pthread_t child;
  pthread_create(&child,0,worker,0);
#if CASE == 2
  value=2;
#endif
#if CASE != 8
# if CASE == 9
  if (pthread_barrier_wait(&gate)==PTHREAD_BARRIER_SERIAL_THREAD) value=3;
# else
  pthread_barrier_wait(&gate);
# endif
#endif
#if CASE == 7
  pthread_barrier_wait(&gate);
#endif
#if CASE == 4
  pthread_mutex_lock(&lock);
#endif
#if CASE != 2
  value=2;
#endif
#if CASE == 4
  pthread_mutex_unlock(&lock);
#endif
#if CASE == 1
  pthread_barrier_wait(&other);
#endif
  return 0;
}
