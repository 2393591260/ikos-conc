#include <pthread.h>
int data;
pthread_t tid;
void *worker(void *arg) { data = 1; return 0; }
void spawn(void) { pthread_create(&tid, 0, worker, 0); }
void initialize(void) { spawn(); }
int main(void) {
  initialize();
  pthread_join(tid, 0);
  data = 2;
  return 0;
}
