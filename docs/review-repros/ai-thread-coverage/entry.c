#include <pthread.h>

#ifndef CASE
#define CASE 0
#endif

static int value;
static void *worker(void *unused) { value = 1; return 0; }
static void *quiet(void *unused) { return 0; }
static void *(*volatile entry)(void *) = worker;
extern void *external_worker(void *);
extern int __VERIFIER_nondet_int(void);

static void start(pthread_t *thread, void *(*routine)(void *)) {
  pthread_create(thread, 0, routine, 0);
}

int main(void) {
  pthread_t thread;
#if CASE == 1 || CASE == 2
  pthread_create(&thread, 0, worker, 0);
#elif CASE == 3
  pthread_create(&thread, 0, external_worker, 0);
#elif CASE == 4
  if (0) pthread_create(&thread, 0, entry, 0);
  value = 2;
  return 0;
#elif CASE == 5
  void *(*chosen)(void *) = __VERIFIER_nondet_int() ? worker : quiet;
  pthread_create(&thread, 0, chosen, 0);
#elif CASE == 6
  start(&thread, entry);
#else
  pthread_create(&thread, 0, entry, 0);
#endif
#if CASE == 2 || CASE == 7
  pthread_join(thread, 0);
#endif
  value = 2;
#if CASE != 2 && CASE != 7
  pthread_join(thread, 0);
#endif
  return 0;
}
