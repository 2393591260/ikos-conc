#include <pthread.h>
extern int __VERIFIER_nondet_int(void);
int values[6] = {1, 1, 2, 2, 3, 3}, maximum;
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
int choose(int start) {
  int local = 0;
  for (int j = start; j < start + 2; ++j) {
    int value = values[j];
    if (value > local) local = value;
  }
  return local;
}
void *worker(void *arg) {
  int start = __VERIFIER_nondet_int();
  if (start != 0 && start != 2 && start != 4) return 0;
  int local = choose(start);
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
