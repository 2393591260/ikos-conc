#include <pthread.h>
__thread int local;
int *published;
void *publisher(void *arg) {
  published = &local;
  local = 1;
  return 0;
}
void *other(void *arg) {
  local = 2;
  return 0;
}
int main(void) {
  pthread_t a,b;
  pthread_create(&a,0,publisher,0);
  pthread_create(&b,0,other,0);
  pthread_join(a,0);
  pthread_join(b,0);
  return 0;
}
