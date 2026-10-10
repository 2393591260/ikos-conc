#include <pthread.h>
extern int __VERIFIER_nondet_int(void);
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
int selector, payload;
int observe(void) { pthread_mutex_lock(&mutex); int result = selector; pthread_mutex_unlock(&mutex); return result; }
void *observer(void *arg) {
  int active = observe();
  if (active) payload = 1;
  return 0;
}
void *writer(void *arg) {
  pthread_mutex_lock(&mutex);
  if (__VERIFIER_nondet_int()) selector = 1;
  pthread_mutex_unlock(&mutex);
  payload = 2;
  return 0;
}
int main(void) {
  pthread_t a, b;
  pthread_create(&a, 0, observer, 0);
  pthread_create(&b, 0, writer, 0);
  return 0;
}
