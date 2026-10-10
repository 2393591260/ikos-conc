#include <pthread.h>
extern int __VERIFIER_nondet_int(void);
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
int flag, data;
void *reader(void *arg) {
  pthread_mutex_lock(&mutex);
  int active = flag;
  pthread_mutex_unlock(&mutex);
  pthread_mutex_lock(&mutex);
  if (active) data = 1;
  pthread_mutex_unlock(&mutex);
  return 0;
}
void *writer(void *arg) {
  pthread_mutex_lock(&mutex);
  if (__VERIFIER_nondet_int()) flag = 1;
  pthread_mutex_unlock(&mutex);
  pthread_mutex_lock(&mutex);
  data = 2;
  pthread_mutex_unlock(&mutex);
  return 0;
}
int main(void) {
  pthread_t a, b;
  pthread_create(&a, 0, reader, 0);
  pthread_create(&b, 0, writer, 0);
  return 0;
}
