#include <pthread.h>
extern int __VERIFIER_nondet_int(void);
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
short selector;
int payload;
void *observer(void *arg) {
  pthread_mutex_lock(&mutex);
  int active = selector;
  pthread_mutex_unlock(&mutex);
  if (active == 255) payload = 1;
  return 0;
}
void mutate(void) { *(signed char *)&selector = -1; }
void *writer(void *arg) {
  pthread_mutex_lock(&mutex);
  mutate();
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
