#include <pthread.h>
extern int __VERIFIER_nondet_int(void);
extern void __VERIFIER_assume(int);
int shared_number;
pthread_mutexattr_t properties;
pthread_mutex_t m;
void *alternate_entry(void *arg) { pthread_mutex_lock(&m); shared_number=1; pthread_mutex_unlock(&m); return 0; }
int main(void) {
  pthread_t t;
  pthread_mutexattr_init(&properties);
  pthread_mutexattr_settype(&properties,PTHREAD_MUTEX_ERRORCHECK);
  pthread_mutex_init(&m,&properties);
  pthread_create(&t,0,alternate_entry,0);
  pthread_mutex_lock(&m);
  pthread_mutex_unlock(&m);
  shared_number=2;
  pthread_join(t,0);
  return 0;
}
