#include <pthread.h>
extern int __VERIFIER_nondet_int(void);
extern void __VERIFIER_assume(int);
int data;
pthread_mutexattr_t attr;
pthread_mutex_t m[3];
void *worker(void *arg) {
  pthread_mutex_lock(&m[1]);
  data=1;
  pthread_mutex_unlock(&m[1]);
  return 0;
}
int main(void) {
  pthread_t t;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_settype(&attr,PTHREAD_MUTEX_ERRORCHECK);
  for(int j=0;j<3;++j) pthread_mutex_init(&m[j],&attr);
  int i=__VERIFIER_nondet_int();
  __VERIFIER_assume(i>=0 && i<3);
  pthread_create(&t,0,worker,0);
  pthread_mutex_lock(&m[1]);
  pthread_mutex_unlock(&m[i]);
  data=2;
  return 0;
}
