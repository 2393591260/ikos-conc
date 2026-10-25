// Initialization must precede every escape, then the actual mutex handoff.
#include <pthread.h>
#include <stdlib.h>
#ifndef CASE
#define CASE 0
#endif
int **slot;
int *escaped;
int sink;
pthread_mutex_t guard = PTHREAD_MUTEX_INITIALIZER;
#if CASE == 1
__attribute__((noinline)) void initialize(int *p) { *p = 1; }
#endif
void *publish(void *unused) {
  int *p = malloc(sizeof(int));
#if CASE == 3
  pthread_mutex_lock(&guard);
  *slot = p;
  pthread_mutex_unlock(&guard);
#elif CASE == 4
  escaped = p;
#endif
#if CASE == 1
  initialize(p);
#else
  *p = 1;
#endif
  pthread_mutex_lock(&guard);
  *slot = p;
  pthread_mutex_unlock(&guard);
  return 0;
}
void *read_slot(void *unused) {
#if CASE != 2
  pthread_mutex_lock(&guard);
#endif
  int *p = *slot;
  if (p) sink = *p;
#if CASE != 2
  pthread_mutex_unlock(&guard);
#endif
  return 0;
}
int main(void) {
  slot = malloc(sizeof(int *));
  *slot = 0;
  pthread_t a, b, c;
  pthread_create(&a, 0, publish, 0);
#if CASE == 5
  pthread_create(&c, 0, publish, 0);
#endif
  pthread_create(&b, 0, read_slot, 0);
  pthread_join(a, 0);
  pthread_join(b, 0);
#if CASE == 5
  pthread_join(c, 0);
#endif
  return 0;
}
