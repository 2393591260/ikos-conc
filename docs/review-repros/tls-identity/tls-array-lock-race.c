#include <pthread.h>
__thread pthread_mutex_t lock[2] = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER};
int data;
void *a(void *arg) {
  pthread_mutex_lock(&lock[1]);
  data = 1;
  pthread_mutex_unlock(&lock[1]);
  return 0;
}
void *b(void *arg) {
  pthread_mutex_lock(&lock[1]);
  data = 2;
  pthread_mutex_unlock(&lock[1]);
  return 0;
}
int main(void) {
  pthread_t x,y;
  pthread_create(&x,0,a,0);
  pthread_create(&y,0,b,0);
  return 0;
}
