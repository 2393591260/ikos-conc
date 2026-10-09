#include <pthread.h>
struct Cell { int value; pthread_mutex_t lock; } cells[3];
void add(struct Cell *cell) {
  pthread_mutex_lock(&cell->lock);
  cell->value++;
  pthread_mutex_unlock(&cell->lock);
}
void *worker(void *arg) {
  for (int i = 0; i < 3; ++i) add(&cells[i]);
  return 0;
}
int main(void) {
  pthread_t thread;
  for (int i = 0; i < 3; ++i) pthread_mutex_init(&cells[i].lock, 0);
  pthread_create(&thread, 0, worker, 0);
  for (int i = 0; i < 3; ++i) add(&cells[i]);
  cells[1].value++;
  return 0;
}
