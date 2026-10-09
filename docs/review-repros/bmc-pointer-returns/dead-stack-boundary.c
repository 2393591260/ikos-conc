#include <pthread.h>
int data;
/* Invalid lifetime must not supply a feasible value to a confirmed race. */
int *escape(void) { int local = 0; return &local; }
void *worker(void *arg) {
  int *pointer = escape();
  if (*pointer == 0) data = 1;
  return 0;
}
int main(void) {
  pthread_t a, b;
  pthread_create(&a, 0, worker, 0);
  pthread_create(&b, 0, worker, 0);
  return 0;
}
