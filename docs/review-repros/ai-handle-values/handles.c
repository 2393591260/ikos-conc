#include <pthread.h>
#include <stdint.h>
#include <string.h>
#ifndef CASE
#define CASE 0
#endif
#ifdef RENAMED
#define data different_data
#define worker different_worker
#endif
static int data;
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
extern pthread_t __VERIFIER_nondet_pthread_t(void);
static void *worker(void *p) {
  (void)p;
#if CASE == 4
  pthread_mutex_lock(&mutex);
#endif
  data=1;
#if CASE == 4
  pthread_mutex_unlock(&mutex);
#endif
  return 0;
}
int main(void) {
#if CASE == 6
  pthread_t t=0;
#else
  pthread_t t;
#endif
  pthread_create(&t,0,worker,0);
#if CASE == 0
  if(t!=(pthread_t)1) data=2;
#elif CASE == 1
  if(t==(pthread_t)1) data=2;
#elif CASE == 2
  if(pthread_self()!=(pthread_t)0xA7E0AD1DULL) data=2;
#elif CASE == 3
  pthread_join(t,0);data=2;return 0;
#elif CASE == 4
  pthread_mutex_lock(&mutex);
  if(t!=(pthread_t)1) data=2;
  pthread_mutex_unlock(&mutex);
#elif CASE == 5
  pthread_t u=t;pthread_join(u,0);data=2;return 0;
#elif CASE == 6
  pthread_join(t,0);data=2;return 0;
#elif CASE == 7
  if(*(unsigned char *)&t==1) data=2;
#elif CASE == 8
  pthread_join((pthread_t)1,0);data=2;return 0;
#elif CASE == 9
  pthread_t u=__VERIFIER_nondet_pthread_t();
  if(u!=(pthread_t)1) return 0;
  pthread_join(u,0);data=2;return 0;
#elif CASE == 10
  pthread_t u;memcpy(&u,&t,sizeof u);
  if(u==(pthread_t)1) data=2;
#elif CASE == 11
  t=(pthread_t)1;
  if(t==(pthread_t)1)data=2;
#elif CASE == 12
  pthread_detach(t);data=2;return 0;
#elif CASE == 13
  pthread_detach(t);
  if(t!=(pthread_t)1)data=2;
  return 0;
#endif
  pthread_join(t,0);
  return 0;
}
