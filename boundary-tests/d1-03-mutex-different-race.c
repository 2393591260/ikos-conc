#include "_boilerplate.h"
int glob;
pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t n = PTHREAD_MUTEX_INITIALIZER;
void *t(void *a){ pthread_mutex_lock(&n); glob++; pthread_mutex_unlock(&n); return 0; }
int main(){ pthread_t id; pthread_create(&id,0,t,0);
  pthread_mutex_lock(&m); glob++; pthread_mutex_unlock(&m); /* RACE: different lock */
  pthread_join(id,0); return 0; }
