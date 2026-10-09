#include <pthread.h>
pthread_t output;
void *child(void *unused) { return 0; }
void *creator(void *unused) {
  pthread_create(&output, 0, child, 0);
  return 0;
}
void *writer(void *unused) { output = 0; return 0; }
int main(void) {
  pthread_t a, b;
  pthread_create(&a, 0, creator, 0);
  pthread_create(&b, 0, writer, 0);
  return 0;
}
