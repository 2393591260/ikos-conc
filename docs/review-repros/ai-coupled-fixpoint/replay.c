#include <pthread.h>
extern int __VERIFIER_nondet_int(void);
static int data[2];
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static void *write_arg(void *arg) { *(int*)arg = 2; return 0; }
static void *write_first(void *unused) { (void)unused; data[0] = 2; return 0; }
static void *spawn_arg(void *arg) { pthread_t child; pthread_create(&child,0,write_arg,arg); pthread_join(child,0); return 0; }
#if CASE == 8
__attribute__((destructor)) static void teardown(void) {
  pthread_t thread; pthread_create(&thread,0,write_first,0); pthread_join(thread,0);
}
#endif
int main(void) {
  pthread_t a,b;
#if CASE == 0 || CASE == 1
  for (int i=0; i<2; ++i) pthread_create(i ? &b : &a,0,write_arg,&data[i]);
  data[CASE] = 1; pthread_join(a,0); pthread_join(b,0);
#elif CASE == 2 || CASE == 3
  pthread_create(&a,0,spawn_arg,&data[0]);
#if CASE == 3
  pthread_join(a,0);
#endif
  data[0] = 1;
#if CASE == 2
  pthread_join(a,0);
#endif
#elif CASE == 4 || CASE == 5
  pthread_create(&a,0,write_first,0);
#if CASE == 4
  if (__VERIFIER_nondet_int()) pthread_join(a,0);
#else
  pthread_join(a,0);
#endif
  pthread_create(&b,0,write_first,0); pthread_join(b,0);
#elif CASE == 6
  pthread_mutex_lock(&mutex);
  pthread_create(&a,0,write_first,0); data[0] = 1; pthread_join(a,0);
  pthread_mutex_unlock(&mutex);
#elif CASE == 7
  pthread_create(&a,0,write_arg,&data[__VERIFIER_nondet_int() ? 0 : 1]);
  data[0] = 1; pthread_join(a,0);
#elif CASE == 9
  pthread_create(&a,0,write_arg,&data[0]); pthread_join(a,0); data[0] = 1;
#endif
  return 0;
}
