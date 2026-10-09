#include <pthread.h>
struct Pair { int x; int y; };
union Storage { struct Pair left; struct { int pad; struct Pair right; } nested; };
union Storage shared;
struct Pair *get_left(void) { return &shared.left; }
struct Pair *get_right(void) { return &shared.nested.right; }
void *a(void *arg) { get_left()->y = 1; return 0; }
void *b(void *arg) { get_right()->x = 1; return 0; }
int main(void) {
  pthread_t x, y;
  pthread_create(&x, 0, a, 0);
  pthread_create(&y, 0, b, 0);
  return 0;
}
