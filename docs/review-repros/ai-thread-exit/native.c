#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>

extern void exit_without_attribute(void *) __asm__("pthread_exit");
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static int continuation;
static void *worker(void *p) {
  (void)p;
  assert(pthread_mutex_lock(&mutex)==0);
  exit_without_attribute(0);
  continuation=1;
  return 0;
}
int main(void) {
  pthread_t t;
  void *status=(void *)1;
  assert(pthread_create(&t,0,worker,0)==0);
  assert(pthread_join(t,&status)==0);
  int locked=pthread_mutex_trylock(&mutex);
  assert(status==0 && continuation==0 && locked==EBUSY);
  printf("NULL status, no continuation, mutex retained: %d\n",locked==EBUSY);
  return 0;
}
