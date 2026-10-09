#include <pthread.h>
__thread int local;
void *child(void *arg) {
  *(int *)arg = 2;
  return 0;
}
void *parent(void *arg) {
  pthread_t t;
  pthread_create(&t,0,child,&local);
  local = 1;
  pthread_join(t,0);
  return 0;
}
int main(void) {
  pthread_t t;
  pthread_create(&t,0,parent,0);
  pthread_join(t,0);
  return 0;
}
