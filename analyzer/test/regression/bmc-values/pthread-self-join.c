// Self-join failure must not become a completion fact in this or a child thread.
#include <pthread.h>
#ifndef CASE
#define CASE 0
#endif
#ifdef WIDE
typedef long long datum;
#else
typedef int datum;
#endif
datum payload;
pthread_t worker_id;
pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;

static pthread_t caller_id(void) { return pthread_self(); }
static void wait_for_caller(void) { pthread_join(caller_id(), 0); }
static void *leaf(void *unused) {
  (void)unused;
  payload = 1;
  return 0;
}
static void *worker(void *unused) {
  (void)unused;
#if CASE == 1 || CASE == 7 || CASE == 13
  pthread_join(pthread_self(), 0);
#elif CASE == 3
  wait_for_caller();
#elif CASE == 5
  pthread_t original = caller_id();
  pthread_t copy = original;
  pthread_join(copy, 0);
#elif CASE == 6
  // main publishes the initialized handle before this acquisition.
  pthread_mutex_lock(&gate);
  pthread_join(worker_id, 0);
  pthread_mutex_unlock(&gate);
#elif CASE == 9
  pthread_t child;
  if (pthread_create(&child, 0, leaf, 0)) return 0;
  pthread_join(child, 0);
#endif
#if CASE == 12 || CASE == 13
  pthread_mutex_lock(&gate);
#endif
  payload = 2;
#if CASE == 12 || CASE == 13
  pthread_mutex_unlock(&gate);
#endif
  return 0;
}
int main(void) {
#if CASE == 0 || CASE == 10 || CASE == 12
  pthread_join(pthread_self(), 0);
#elif CASE == 2
  wait_for_caller();
#elif CASE == 4
  pthread_t original = caller_id();
  pthread_t copy = original;
  pthread_join(copy, 0);
#endif
#if CASE == 10
  pthread_t previous;
  if (pthread_create(&previous, 0, leaf, 0)) return 0;
  pthread_join(previous, 0);
#elif CASE == 6
  pthread_mutex_lock(&gate);
#endif
  if (pthread_create(&worker_id, 0, worker, 0)) return 0;
#if CASE == 6
  pthread_mutex_unlock(&gate);
#elif CASE == 7
  pthread_t another;
  if (pthread_create(&another, 0, worker, 0)) return 0;
#elif CASE == 11
  pthread_join(pthread_self(), 0);
#elif CASE == 8 || CASE == 9
  pthread_join(worker_id, 0);
#endif
#if CASE == 12 || CASE == 13
  pthread_mutex_lock(&gate);
#endif
#if CASE != 9
  payload = 3;
#endif
#if CASE == 12 || CASE == 13
  pthread_mutex_unlock(&gate);
#endif
#if CASE == 0 || CASE == 2 || CASE == 4 || CASE == 10 || CASE == 11 || CASE == 12
  pthread_join(worker_id, 0);
#endif
  // Worker-self-join cases have no competing joiner for that same thread.
  return 0;
}
