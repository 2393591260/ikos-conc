#include <pthread.h>
int data, entered;
void *worker(void *arg) { data = 1; return 0; }
int main(void) {
  pthread_t tid;
  pthread_create(&tid, 0, worker, 0);
  if (!entered) { entered = 1; main(); }
  return 0;
}
