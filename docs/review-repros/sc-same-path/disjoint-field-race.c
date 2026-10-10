#include <pthread.h>
struct Pair {int left, right;} cells;
int data;
void *a(void *arg) {cells.left = 3; if (cells.left == 3) data = 1; return 0;}
void *b(void *arg) {cells.right = 7; data = 2; return 0;}
int main(void) {
  pthread_t x, y;
  pthread_create(&x, 0, a, 0);
  pthread_create(&y, 0, b, 0);
  return 0;
}
