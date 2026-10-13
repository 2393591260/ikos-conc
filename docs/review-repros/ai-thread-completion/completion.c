#include <pthread.h>
#ifndef CASE
#define CASE 0
#endif
#ifndef COUNT
#define COUNT 4
#endif
static int shared;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
extern int __VERIFIER_nondet_int(void);
extern void observe(void *);
static void *worker(void *arg) {
#if CASE == 8
  pthread_detach(pthread_self());
#endif
  pthread_mutex_lock(&mutex); shared = 1; pthread_mutex_unlock(&mutex);
  return arg;
}
#if CASE == 11
static pthread_t constructor_handle;
__attribute__((constructor)) static void before_main(void) {
  pthread_create(&constructor_handle, 0, worker, 0);
}
#endif
int main(void) {
  pthread_t handles[COUNT];
#if CASE == 5
  int count = __VERIFIER_nondet_int();
  if (count < 0 || count > COUNT) return 0;
#else
  int count = COUNT;
#endif
#if CASE == 7
  observe(handles);
#endif
  for (int i=0; i<count; ++i) pthread_create(&handles[i], 0, worker, 0);
#if CASE == 3
  pthread_create(&handles[0], 0, worker, 0);
#elif CASE == 4
  handles[COUNT-1] = handles[0];
#elif CASE == 6
  ((unsigned char *)&handles[COUNT-1])[0] = 1;
#endif
#if CASE == 10
  // An untouched slot must not acquire another element's MAY origin.
  ++count;
#endif
  for (int j=0; j < (CASE == 1 ? count-1 : count); j += (CASE == 2 ? 2 : 1))
    pthread_join(handles[j], 0);
#if CASE == 9
  pthread_create(&handles[0], 0, worker, 0); // a new live generation
#endif
  return shared;
}
