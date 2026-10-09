#include <pthread.h>
int shared;
void *worker(void *arg) { shared = 1; return 0; }
void launch_and_join(void) {
  pthread_t tid;
  pthread_create(&tid, 0, worker, 0);
  pthread_join(tid, 0);
}
int main(void) { launch_and_join(); launch_and_join(); return 0; }
