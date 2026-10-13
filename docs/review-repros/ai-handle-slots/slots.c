// Opaque typed slots do not imply universal dynamic-instance completion.
#include <pthread.h>
#ifndef CASE
#define CASE 0
#endif
static int data;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static void *worker(void *arg) {
  pthread_mutex_lock(&lock); data = 1; pthread_mutex_unlock(&lock);
  return arg;
}
extern void overwrite(void *);
int main(void) {
  pthread_t slots[4];
#if CASE == 1 || CASE == 2
  for (int i=0; i<4; ++i) pthread_create(&slots[i], 0, worker, 0);
#else
  pthread_create(&slots[0], 0, worker, 0);
  pthread_create(&slots[1], 0, worker, 0);
  pthread_create(&slots[2], 0, worker, 0);
  pthread_create(&slots[3], 0, worker, 0);
#endif
#if CASE == 2 || CASE == 4
  pthread_create(&slots[0], 0, worker, 0);
#elif CASE == 5
  slots[3] = slots[0];
#elif CASE == 6
  ((unsigned char *)&slots[3])[0] = 1;
#elif CASE == 7
  overwrite(&slots[3]);
#elif CASE == 8
  slots[3] = (pthread_t)1;
#elif CASE == 9
  // Valid identity, wrong slot origin: the fourth creation is not joined.
  pthread_t *alias = &slots[3];
  *alias = slots[0];
#endif
  pthread_join(slots[0], 0);
  pthread_join(slots[1], 0);
  pthread_join(slots[2], 0);
#if CASE != 3
  pthread_join(slots[3], 0);
#endif
  return data;
}
