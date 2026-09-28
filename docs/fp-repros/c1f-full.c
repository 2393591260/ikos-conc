#include <pthread.h>
int global;
pthread_mutex_t __global_lock = PTHREAD_MUTEX_INITIALIZER, mutex1 = PTHREAD_MUTEX_INITIALIZER, mutex2 = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t *mp = &mutex1;
void __VERIFIER_assert(int c){ if(!c) __builtin_abort(); }
void *t_fun(void *arg){
  pthread_mutex_t *mp1;
  int *p = &global;
  mp1 = mp;
  pthread_mutex_lock(mp);
  pthread_mutex_lock(&__global_lock); global++; pthread_mutex_unlock(&__global_lock);
  pthread_mutex_lock(&__global_lock); global--; pthread_mutex_unlock(&__global_lock);
  pthread_mutex_unlock(mp);
  return 0; }
int main(){
  pthread_mutex_t *mp1;
  mp = &mutex1;
  pthread_t t_ids[10]; for (int i=0; i<10; i++) pthread_create(&t_ids[i], 0, t_fun, 0);
  mp1 = mp;
  pthread_mutex_lock(mp);
  pthread_mutex_lock(&__global_lock); __VERIFIER_assert(global == 0); pthread_mutex_unlock(&__global_lock);
  pthread_mutex_unlock(mp);
  for (int i=0; i<10; i++) pthread_join(t_ids[i], 0);
  return 0; }
