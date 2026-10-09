#include <pthread.h>
int data[2];
int *location(int index) { return &data[index]; }
void *a(void *arg) {
  *location(0) = 1;
  *location(1) = 2;
  return 0;
}
void *b(void *arg) { *location(1) = 3; return 0; }
int main(void) {
  pthread_t x, y;
  pthread_create(&x, 0, a, 0);
  pthread_create(&y, 0, b, 0);
  return 0;
}
