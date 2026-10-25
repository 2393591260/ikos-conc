#include <pthread.h>
#ifndef CASE
#define CASE 0
#endif
int payload;
pthread_mutex_t guard = PTHREAD_MUTEX_INITIALIZER;
void *worker(void *unused) {
  pthread_mutex_lock(&guard); payload++; pthread_mutex_unlock(&guard); return 0;
}
void *inert(void *unused) { return 0; }
extern int __VERIFIER_nondet_int(void);
int main(void) {
  pthread_t ids[2];
  for (int i=0; i<2; ++i) pthread_create(&ids[i],0,worker,0);
#if CASE == 2
  for (int i=0; i<2; ++i) pthread_create(&ids[i],0,inert,0);
#endif
  for (int i=0; i<2; ++i) {
#if CASE == 1
    if (i==0) pthread_join(ids[i],0);
#elif CASE == 3
    pthread_join((unsigned short)ids[i],0);
#elif CASE == 5
    if (__VERIFIER_nondet_int()) pthread_join(ids[i],0);
#else
    pthread_join(ids[i],0);
#endif
  }
#if CASE == 4
  pthread_t next;
  pthread_create(&next,0,worker,0);
#endif
  return payload;
}
