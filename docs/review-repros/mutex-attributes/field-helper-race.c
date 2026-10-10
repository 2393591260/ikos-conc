#include <pthread.h>
extern int __VERIFIER_nondet_int(void);
extern void __VERIFIER_assume(int);
int data;
struct Controls { pthread_mutexattr_t attr; pthread_mutex_t m; };
struct Controls controls;
void *worker(void *arg) { pthread_mutex_lock(&controls.m); data=1; pthread_mutex_unlock(&controls.m); return 0; }
void setup(void) {
  pthread_mutexattr_init(&controls.attr);
  pthread_mutexattr_settype(&controls.attr,PTHREAD_MUTEX_ERRORCHECK);
  pthread_mutex_init(&controls.m,&controls.attr);
}
int main(void) {
  pthread_t t;
  setup();
  pthread_create(&t,0,worker,0);
  pthread_mutex_lock(&controls.m);
  pthread_mutex_unlock(&controls.m);
  data=2;
  pthread_join(t,0);
  return 0;
}
