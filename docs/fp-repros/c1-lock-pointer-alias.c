#include <pthread.h>
int global;
pthread_mutex_t mutex1 = PTHREAD_MUTEX_INITIALIZER, mutex2 = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t *mp = &mutex1;
void *t(void *x){ pthread_mutex_lock(mp); global++; pthread_mutex_unlock(mp); return 0; }
int main(){ pthread_t t1;
  pthread_create(&t1,0,t,0);
  pthread_mutex_lock(mp); global++; pthread_mutex_unlock(mp);
  pthread_join(t1,0); return 0; }
