#include <pthread.h>
int cells[3], shared;
void initialize(void) {
  for (int k = 2; k >= 0; --k) cells[k] = k + 3;
}
void *run(void *arg) {
  if (cells[2] == 5) shared = 1;
  return 0;
}
int main(void) {
  initialize();
  pthread_t a, b;
  pthread_create(&a, 0, run, 0);
  pthread_create(&b, 0, run, 0);
  return 0;
}
