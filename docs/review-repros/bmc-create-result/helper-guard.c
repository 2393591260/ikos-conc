#include <pthread.h>
int data;
void *worker(void *arg) {
  data = 1;
  return 0;
}
int start(pthread_t *tid) {
  return pthread_create(tid, 0, worker, 0);
}
int main(void) {
  pthread_t tid;
  if (start(&tid) != 0) return 1;
  data = 2;
  pthread_join(tid, 0);
  return 0;
}
