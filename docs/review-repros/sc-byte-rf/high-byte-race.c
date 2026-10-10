#include <pthread.h>
extern int __VERIFIER_nondet_int(void);
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
short flag;
int data;
void *reader(void *arg) {
  pthread_mutex_lock(&mutex);
  int active = flag;
  pthread_mutex_unlock(&mutex);
  if (active == 256) data = 1;
  return 0;
}
void *writer(void *arg) {
  pthread_mutex_lock(&mutex);
  *((signed char *)&flag + 1) = 1;
  pthread_mutex_unlock(&mutex);
  data = 2;
  return 0;
}
int main(void) {
  pthread_t a, b;
  pthread_create(&a, 0, reader, 0);
  pthread_create(&b, 0, writer, 0);
  return 0;
}
