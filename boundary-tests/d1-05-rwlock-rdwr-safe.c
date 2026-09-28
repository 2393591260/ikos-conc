#include "_boilerplate.h"
int glob;
pthread_rwlock_t rw = PTHREAD_RWLOCK_INITIALIZER;
void *t(void *a){ pthread_rwlock_wrlock(&rw); glob++; pthread_rwlock_unlock(&rw); return 0; }
int main(){ pthread_t id; pthread_create(&id,0,t,0);
  pthread_rwlock_rdlock(&rw); glob++; pthread_rwlock_unlock(&rw); /* rd vs wr => excluded */
  pthread_join(id,0); return 0; }
