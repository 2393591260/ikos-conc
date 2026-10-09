#include <pthread.h>
struct Pair { int x; int y; } shared;
void *a(void *arg) { shared.y = 1; return 0; }
void *b(void *arg) { shared.x = 1; return 0; }
int main(void) {
  pthread_t x, y;
  pthread_create(&x, 0, a, 0);
  pthread_create(&y, 0, b, 0);
  return 0;
}
