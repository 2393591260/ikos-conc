#include <pthread.h>
unsigned int state;
int data;
void *a(void *arg) {state = ~0u; if (*(int *)&state == -1) data = 1; return 0;}
void *b(void *arg) {data = 2; return 0;}
int main(void) {
  pthread_t x, y;
  pthread_create(&x, 0, a, 0);
  pthread_create(&y, 0, b, 0);
  return 0;
}
