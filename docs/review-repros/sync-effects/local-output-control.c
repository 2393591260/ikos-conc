#include <pthread.h>
pthread_mutexattr_t attr;
void *getter(void *unused) {
  int output;
  pthread_mutexattr_gettype(&attr, &output);
  return 0;
}
int main(void) {
  pthread_t a;
  pthread_mutexattr_init(&attr);
  pthread_create(&a, 0, getter, 0);
  return 0;
}
