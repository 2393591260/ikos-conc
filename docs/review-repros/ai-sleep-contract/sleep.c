#include <pthread.h>
#include <signal.h>
#ifndef CASE
#define CASE 0
#endif
#ifdef RENAMED
#define data changed_data
#define worker changed_worker
#endif
#if CASE == 3
extern unsigned sleep(unsigned, unsigned);
#elif CASE == 9
extern unsigned sleep(unsigned, ...);
#elif CASE == 5
static unsigned own_sleep(unsigned) __asm__("sleep");
#else
extern unsigned sleep(unsigned);
#endif
extern unsigned sleep_similar(unsigned);
extern unsigned __VERIFIER_nondet_uint(void);
static int data;
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
#if CASE == 5
static unsigned own_sleep(unsigned seconds) {(void)seconds;data=2;return 0;}
#else
static unsigned wait_helper(unsigned seconds) {
#if CASE == 3
  return sleep(seconds,0);
#else
  return sleep(seconds);
#endif
}
#endif
static void signal_handler(int n) {(void)n;data=3;}
static void *worker(void *arg) {
  (void)arg;
#if CASE == 0 || CASE == 7
  pthread_mutex_lock(&mutex);
#endif
#if CASE != 2
  data=1;
#endif
#if CASE == 0 || CASE == 7
  pthread_mutex_unlock(&mutex);
#endif
  return 0;
}
int main(void) {
  pthread_t thread;
  pthread_create(&thread,0,worker,0);
#if CASE == 0 || CASE == 7
  pthread_mutex_lock(&mutex);
#endif
#if CASE == 4
  sleep_similar(0);
#elif CASE == 5
  own_sleep(0);
#elif CASE == 6
  signal(SIGALRM,signal_handler);
  wait_helper(1);
#elif CASE == 2
  if(wait_helper(0)>0) data=2;
#elif CASE == 8
  if(wait_helper(1)>0) data=2;
#elif CASE == 7
  wait_helper(__VERIFIER_nondet_uint());
#else
  wait_helper(0);
#endif
#if CASE == 0 || CASE == 1 || CASE == 7
  data=2;
#endif
#if CASE == 0 || CASE == 7
  pthread_mutex_unlock(&mutex);
#endif
  pthread_join(thread,0);
  return 0;
}
