#ifndef CASE
#define CASE 0
#endif
#if CASE == 6
typedef unsigned long pthread_t;
extern int pthread_create(pthread_t *, const void *, void *(*)(void *), void *);
extern int pthread_join(pthread_t, void **);
#else
#include <pthread.h>
#endif
#if CASE == 9
extern int wrong_exit(void *) __asm__("pthread_exit");
#endif
#if CASE == 10
extern void exit_without_attribute(void *) __asm__("pthread_exit");
#endif
static int data;
#if CASE != 6
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_key_t key;
static void cleanup(void *p) {(void)p;data=3;}
#endif
#if CASE == 6
static void own_exit(void *) __asm__("pthread_exit");
static void own_exit(void *p) {(void)p;data=1;}
#else
static void exit_helper(void) {pthread_exit(0);}
#endif
static void *worker(void *p) {
  (void)p;
#if CASE == 0 || CASE == 8
  pthread_mutex_lock(&mutex);data=1;
#if CASE == 0
  pthread_mutex_unlock(&mutex);
#endif
  exit_helper();
#elif CASE == 1 || CASE == 7
  data=1;exit_helper();
#elif CASE == 2
  exit_helper();data=1;
#elif CASE == 3
  pthread_cleanup_push(cleanup,0);
  exit_helper();
  pthread_cleanup_pop(0);
#elif CASE == 4
  pthread_key_create(&key,cleanup);pthread_setspecific(key,(void *)1);exit_helper();
#elif CASE == 5
  pthread_exit(&data);
#elif CASE == 6
  own_exit(0);
#elif CASE == 9
  wrong_exit(0);
#elif CASE == 10
  exit_without_attribute(0);data=1;
#endif
  return 0;
}
int main(void) {
  pthread_t t;pthread_create(&t,0,worker,0);
#if CASE == 0 || CASE == 8
  pthread_mutex_lock(&mutex);data=2;pthread_mutex_unlock(&mutex);
#else
  data=2;
#endif
#if CASE == 7
  exit_helper();
#else
  pthread_join(t,0);
#endif
  return 0;
}
