#include <pthread.h>
#include <stdlib.h>
#ifndef CASE
#define CASE 0
#endif
#if __SIZEOF_POINTER__ == 8
#define DISTANCE 4294967296ULL
#else
#define DISTANCE 256U
#endif
#if CASE == 4
#define START 16U
#else
#define START 0U
#endif
static char *base;
static int data;
static pthread_mutex_t outer=PTHREAD_MUTEX_INITIALIZER;
static void *first(void *p) {
  (void)p;
#if CASE == 2
  pthread_mutex_lock(&outer);
#endif
  pthread_mutex_lock((pthread_mutex_t*)(base+START)); data=1;
  pthread_mutex_unlock((pthread_mutex_t*)(base+START));
#if CASE == 2
  pthread_mutex_unlock(&outer);
#endif
  return 0;
}
static void *second(void *p) {
  (void)p;
#if CASE == 1
  pthread_mutex_t *lock=(pthread_mutex_t*)(base+START);
#else
  pthread_mutex_t *lock=(pthread_mutex_t*)(base+START+DISTANCE);
#endif
#if CASE == 2
  pthread_mutex_lock(&outer);
#endif
  pthread_mutex_lock(lock); data=2; pthread_mutex_unlock(lock);
#if CASE == 2
  pthread_mutex_unlock(&outer);
#endif
  return 0;
}
int main(void) {
  pthread_t a,b;
  base=malloc(START+DISTANCE+sizeof(pthread_mutex_t)); if(!base) return 0;
  pthread_mutex_init((pthread_mutex_t*)(base+START),0);
#if CASE != 1
  pthread_mutex_init((pthread_mutex_t*)(base+START+DISTANCE),0);
#endif
  pthread_create(&a,0,first,0);
#if CASE == 3
  pthread_join(a,0);
#endif
  pthread_create(&b,0,second,0);
#if CASE != 3
  pthread_join(a,0);
#endif
  pthread_join(b,0); free(base); return 0;
}
