#include <pthread.h>
unsigned int x;
void* a(void* arg) { x = 1; return 0; }
void* b(void* arg) { ((unsigned char*)&x)[0] = 2; return 0; }
int main(void) {
  pthread_t ta, tb;
  pthread_create(&ta, 0, a, 0);
  pthread_create(&tb, 0, b, 0);
  pthread_join(ta, 0);
  pthread_join(tb, 0);
  return 0;
}
