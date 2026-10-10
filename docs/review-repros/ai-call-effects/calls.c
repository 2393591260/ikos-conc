#include <pthread.h>
#include <stddef.h>
#ifndef CASE
#define CASE 0
#endif
#ifdef RENAMED
#define data other_data
#define opaque_effect other_api
#define worker renamed_worker
#endif
int data;
static pthread_mutex_t mutex;
#if CASE == 1
static void opaque_effect(void) { data=2; }
#elif CASE == 6
static int opaque_effect(void) { return 7; }
#else
extern void opaque_effect(void);
#endif
#if CASE == 5
extern int __VERIFIER_nondet_int(int);
#elif CASE == 15
extern void __VERIFIER_nondet_int(void);
#else
extern int __VERIFIER_nondet_int(void);
#endif
extern int __VERIFIER_nondet_int_fake(void);
extern void __VERIFIER_nondet_memory(void *, size_t);
extern double __VERIFIER_nondet_double(void);
extern long __VERIFIER_nondet_long();
extern void __VERIFIER_assume(int);
extern void *__VERIFIER_nondet_pointer(void);
static void setup(pthread_mutexattr_t *attr) { pthread_mutex_init(&mutex,attr); }
static void *worker(void *arg) {
  (void)arg;
#if CASE == 10 || CASE == 11 || CASE >= 16
  pthread_mutex_lock(&mutex);
#endif
  data=1;
#if CASE == 10 || CASE == 11 || CASE >= 16
  pthread_mutex_unlock(&mutex);
#endif
  return 0;
}
static void callback(void) { data=2; }
extern void invoke_callback(void (*)(void));
int main(void) {
#if CASE == 8
  opaque_effect(); return 0;
#else
  pthread_t thread;
#if CASE == 10
  pthread_mutex_init(&mutex,0);
#elif CASE == 16
  pthread_mutexattr_t *attr=0; setup(attr);
#elif CASE == 17
  pthread_mutexattr_t *attr=__VERIFIER_nondet_pointer();
  if(attr) return 0;
  setup(attr);
#elif CASE == 11
  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutex_init(&mutex,&attr);
#endif
  pthread_create(&thread,0,worker,0);
#if CASE == 0 || CASE == 1 || CASE == 6
  opaque_effect();
#elif CASE == 2
  if(__VERIFIER_nondet_int()) data=2;
#elif CASE == 3
  __VERIFIER_nondet_int();
#elif CASE == 4
  __VERIFIER_nondet_int_fake();
#elif CASE == 5
  __VERIFIER_nondet_int(0);
#elif CASE == 7
  if(0) opaque_effect();
#elif CASE == 9
  invoke_callback(callback);
#elif CASE == 12
  __VERIFIER_nondet_memory(&data,sizeof data);
#elif CASE == 13
  __VERIFIER_assume(1);
#elif CASE == 14
  __VERIFIER_nondet_double(); __VERIFIER_nondet_long();
#elif CASE == 15
  __VERIFIER_nondet_int();
#endif
  pthread_join(thread,0);
#if CASE == 10 || CASE == 11 || CASE >= 16
  pthread_mutex_destroy(&mutex);
#endif
  return 0;
#endif
}
