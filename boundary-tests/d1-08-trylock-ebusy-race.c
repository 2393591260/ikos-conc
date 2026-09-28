#include "_boilerplate.h"
int glob;
pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
void *t(void *a){ pthread_mutex_lock(&m); glob++; pthread_mutex_unlock(&m); return 0; }
int main(){ pthread_t id; pthread_create(&id,0,t,0);
  if(pthread_mutex_trylock(&m)!=0){ glob++; }  /* RACE (EBUSY path, no lock held) */
  pthread_join(id,0); return 0; }
