#include <pthread.h>
extern int __VERIFIER_nondet_int(void);
int left, right;
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
void *worker(void *arg) {
  pthread_mutex_lock(&lock);
  right = 9;
  pthread_mutex_unlock(&lock);
  return 0;
}
int main(void) {
  pthread_t t;
  int *chosen = __VERIFIER_nondet_int() ? &left : &right;
  pthread_create(&t, 0, worker, 0);
  pthread_mutex_lock(&lock);
  *chosen = 7;
  pthread_mutex_unlock(&lock);
  pthread_join(t, 0);
  return 0;
}
