#include "_boilerplate.h"
int glob, other;
pthread_barrier_t bar;
void *t(void *x){ glob = 1; pthread_barrier_wait(&bar); return 0; }  /* write BEFORE barrier */
int main(){ pthread_t id; pthread_barrier_init(&bar,0,2);
  pthread_create(&id,0,t,0);
  pthread_barrier_wait(&bar);
  other = glob;             /* read AFTER barrier => HB (theory) */
  pthread_join(id,0); return 0; }
