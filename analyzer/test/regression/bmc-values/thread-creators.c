// One entry can have different actual creators; join facts stay instance-bound.
#include <pthread.h>
#ifndef CASE
#define CASE 0
#endif
#ifdef WIDE
typedef long long datum;
#else
typedef int datum;
#endif
volatile datum shared;
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static void *reader(void *arg) {
#if CASE == 6
  pthread_mutex_lock(&gate);
#endif
  datum value = shared;
  (void)value;
#if CASE == 6
  pthread_mutex_unlock(&gate);
#endif
  return 0;
}
static void start_reader(pthread_t *handle) {
  pthread_create(handle, 0, reader, 0);
}
static void *creator(void *arg) {
  pthread_t child;
#if CASE == 8
  start_reader(&child);
#else
  pthread_create(&child, 0, reader, 0);
#endif
#if CASE != 3
  pthread_join(child, 0);
#endif
  return 0;
}
int main(void) {
  pthread_t direct, parent;
#if CASE == 10
  pthread_create(&direct, 0, creator, 0);
  pthread_create(&parent, 0, creator, 0);
  pthread_join(direct, 0);
  shared = 1;
  pthread_join(parent, 0);
  return 0;
#elif CASE == 11
  pthread_create(&parent, 0, creator, 0);
  pthread_join(parent, 0);
  pthread_create(&direct, 0, reader, 0);
  shared = 1;
  pthread_join(direct, 0);
  return 0;
#else
#if CASE == 2
  shared = 1;
#endif
#if CASE != 4
#if CASE == 8
  start_reader(&direct);
#else
  pthread_create(&direct, 0, reader, 0);
#endif
#if CASE != 9
  pthread_join(direct, 0);
#endif
#endif
#if CASE == 7
  shared = 1;
  pthread_create(&parent, 0, reader, 0);
  pthread_join(parent, 0);
#else
#if CASE != 5
  pthread_create(&parent, 0, creator, 0);
#if CASE == 1 || CASE == 3
  pthread_join(parent, 0);
#endif
#endif
#if CASE == 6
  pthread_mutex_lock(&gate);
#endif
#if CASE != 2
  shared = 1;
#endif
#if CASE == 6
  pthread_mutex_unlock(&gate);
#endif
#if CASE == 5
  pthread_create(&parent, 0, creator, 0);
#endif
#if CASE != 1 && CASE != 3
  pthread_join(parent, 0);
#endif
#endif
#if CASE == 9
  pthread_join(direct, 0);
#endif
  return 0;
#endif
}
