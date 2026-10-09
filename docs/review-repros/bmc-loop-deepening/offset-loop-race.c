#include <pthread.h>
extern int __VERIFIER_nondet_int(void);
int values[6] = {1, 1, 2, 2, 3, 3}, maximum;
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
void *worker(void *arg) {
  int offset = __VERIFIER_nondet_int();
  if (offset != 0 && offset != 2 && offset != 4) return 0;
  int local = 0;
  for (int i = offset; i < offset + 2; ++i) {
    int value = values[i];
    if (value > local) local = value;
  }
  pthread_mutex_lock(&lock);
  if (local > maximum) maximum = local;
  pthread_mutex_unlock(&lock);
  if (maximum < local) return (void *)1;
  return 0;
}
int main(void) {
  pthread_t a, b;
  pthread_create(&a, 0, worker, 0);
  pthread_create(&b, 0, worker, 0);
  return 0;
}
