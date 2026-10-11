#include <pthread.h>
extern int __VERIFIER_nondet_int(void);
#if CASE >= 10
static pthread_mutex_t mutex;
#else
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
#endif
static pthread_rwlock_t rwlock = PTHREAD_RWLOCK_INITIALIZER;
static int payload;
static void write_cell(void) { payload = 1; }
static void release(void) { pthread_mutex_unlock(&mutex); }
static void *worker(void *unused) {
#if CASE == 5
  pthread_rwlock_rdlock(&rwlock);
  payload = 2;
  pthread_rwlock_unlock(&rwlock);
#else
  pthread_mutex_lock(&mutex);
  payload = 2;
  pthread_mutex_unlock(&mutex);
#endif
  return 0;
}
int main(void) {
  pthread_t t;
  int g = __VERIFIER_nondet_int();
  int h = __VERIFIER_nondet_int();
#if CASE >= 10
  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_settype(&attr, CASE == 10 ? PTHREAD_MUTEX_ERRORCHECK : PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&mutex, &attr);
  pthread_mutexattr_destroy(&attr);
#endif
  pthread_create(&t, 0, worker, 0);
#if CASE == 0
  if (g) {
    pthread_mutex_lock(&mutex);
    if (h) payload = 1;
    pthread_mutex_unlock(&mutex);
  }
#elif CASE == 1
  if (g) pthread_mutex_lock(&mutex);
  if (h) payload = 1;
  if (g) pthread_mutex_unlock(&mutex);
#elif CASE == 2
  if (g) {
    pthread_mutex_lock(&mutex);
    pthread_mutex_unlock(&mutex);
    if (h) payload = 1;
  }
#elif CASE == 3
  if (!g) {
    pthread_mutex_lock(&mutex);
    if (h) payload = 1;
    pthread_mutex_unlock(&mutex);
  }
#elif CASE == 4
  int rc = pthread_mutex_trylock(&mutex);
  if (h) payload = 1;
  if (rc == 0) pthread_mutex_unlock(&mutex);
#elif CASE == 5
  if (g) {
    pthread_rwlock_rdlock(&rwlock);
    if (h) payload = 1;
    pthread_rwlock_unlock(&rwlock);
  }
#elif CASE == 6
  if (g) {
    pthread_mutex_lock(&mutex);
    if (h) write_cell();
    pthread_mutex_unlock(&mutex);
  }
#elif CASE == 7
  if (g) pthread_mutex_lock(&mutex);
  if (g) payload = 1;
  if (g) pthread_mutex_unlock(&mutex);
#elif CASE == 8
  if (g) {
    pthread_mutex_lock(&mutex);
    release();
    if (h) payload = 1;
  }
#elif CASE == 9
  if (g) {
    pthread_mutex_lock(&mutex);
    while (h) {
      payload = 1;
      h = __VERIFIER_nondet_int();
    }
    pthread_mutex_unlock(&mutex);
  }
#elif CASE == 10
  pthread_mutex_lock(&mutex);
  if (g) {
    pthread_mutex_lock(&mutex); /* EDEADLK: no second acquisition. */
    pthread_mutex_unlock(&mutex);
    if (h) payload = 1;
  } else {
    pthread_mutex_unlock(&mutex);
  }
#elif CASE == 11
  pthread_mutex_lock(&mutex);
  if (g) {
    pthread_mutex_lock(&mutex);
    pthread_mutex_unlock(&mutex);
    if (h) payload = 1; /* Recursive depth remains one. */
  }
  pthread_mutex_unlock(&mutex);
#endif
  pthread_join(t, 0);
  return 0;
}
