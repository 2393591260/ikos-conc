#include <pthread.h>
#include <stdlib.h>
struct S { int v; };
struct S* p;
pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
void* a(void* arg) {
  p->v = 1;
  pthread_mutex_lock(&m);
  p->v = 2;
  pthread_mutex_unlock(&m);
  return 0;
}
void* b(void* arg) {
  pthread_mutex_lock(&m);
  p->v = 3;
  pthread_mutex_unlock(&m);
  return 0;
}
int main(void) {
  pthread_t ta, tb;
  p = malloc(sizeof *p);
  if (!p) return 0;
  p->v = 0;
  pthread_create(&ta, 0, a, 0);
  pthread_create(&tb, 0, b, 0);
  pthread_join(ta, 0);
  pthread_join(tb, 0);
  free(p);
  return 0;
}
