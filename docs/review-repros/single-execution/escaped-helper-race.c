#include <pthread.h>
int shared;
void *worker(void *arg) { shared = 1; return 0; }
void launch(void) {
  pthread_t tid;
  pthread_create(&tid, 0, worker, 0);
}
void (*callbacks[2])(void) = {launch, launch};
int main(void) { callbacks[0](); callbacks[1](); return 0; }
