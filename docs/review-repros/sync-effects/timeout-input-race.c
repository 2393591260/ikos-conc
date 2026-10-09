#include <pthread.h>
#include <semaphore.h>
#include <time.h>
sem_t sem;
struct timespec deadline = {0, 0};
void *waiter(void *unused) { sem_timedwait(&sem, &deadline); return 0; }
void *writer(void *unused) { deadline.tv_nsec = 0; return 0; }
int main(void) {
  pthread_t a, b;
  sem_init(&sem, 0, 0);
  pthread_create(&a, 0, waiter, 0);
  pthread_create(&b, 0, writer, 0);
  return 0;
}
