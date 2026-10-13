#include <pthread.h>
extern int __VERIFIER_nondet_int(void);
static int data, export_probe;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
#ifndef JOINS
#define JOINS 3
#endif
static void *worker(void *arg) {
  pthread_mutex_lock(&mutex);
  data = __VERIFIER_nondet_int();
  pthread_mutex_unlock(&mutex);
  /* Export a real worker/worker BMC certificate; the constructed schedule
     chooses this branch false in every completed child. */
  if (__VERIFIER_nondet_int()) export_probe = 1;
  return 0;
}
int main(void) {
  pthread_t handles[4];
  for (int i = 0; i < 4; ++i) pthread_create(&handles[i], 0, worker, 0);
  for (int i = 0; i < JOINS; ++i) pthread_join(handles[i], 0);
  return data;
}
