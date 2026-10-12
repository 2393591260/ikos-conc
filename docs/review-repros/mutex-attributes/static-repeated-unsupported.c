#define _GNU_SOURCE
#include <pthread.h>

#ifdef RECURSIVE
static pthread_mutex_t gate = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
#else
static pthread_mutex_t gate = PTHREAD_ERRORCHECK_MUTEX_INITIALIZER_NP;
#endif
static int cell;

static void *worker(void *arg) {
  pthread_mutex_lock(&gate);
  cell = 1;
  pthread_mutex_unlock(&gate);
  return 0;
}

int main(void) {
  pthread_t child;
  pthread_create(&child, 0, worker, 0);
  pthread_mutex_lock(&gate);
  int result = pthread_mutex_lock(&gate);
  if (result != 0) {
    pthread_mutex_unlock(&gate);
    cell = 2; // ERRORCHECK failed: this write no longer owns the mutex.
  } else {
    pthread_mutex_unlock(&gate);
    pthread_mutex_unlock(&gate);
  }
  pthread_join(child, 0);
  return 0;
}
