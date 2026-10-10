#include <pthread.h>
extern int __VERIFIER_nondet_int(void);
extern void __VERIFIER_assume(int);
int data;
pthread_mutexattr_t attr;
pthread_mutex_t m;
void *worker(void *arg) { pthread_mutex_lock(&m); data=1; pthread_mutex_unlock(&m); return 0; }
int main(void) {
  pthread_t t;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_settype(&attr,PTHREAD_MUTEX_ERRORCHECK);
  pthread_mutex_init(&m,&attr);
  pthread_mutexattr_destroy(&attr);
  pthread_create(&t,0,worker,0);
  pthread_mutex_lock(&m);
  pthread_mutex_unlock(&m);
  data=2;
  pthread_join(t,0);
  return 0;
}
