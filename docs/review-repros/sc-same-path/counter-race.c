#include <pthread.h>
int counter, data;
void *a(void *arg) {
  for (counter = 0; counter < 3; ++counter) {}
  if (counter == 3) data = 1;
  return 0;
}
void *b(void *arg) {data = 2; return 0;}
int main(void) {
  pthread_t x, y;
  pthread_create(&x, 0, a, 0);
  pthread_create(&y, 0, b, 0);
  return 0;
}
