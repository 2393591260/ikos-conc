#include <pthread.h>
#include <stdlib.h>
extern int __VERIFIER_nondet_int(void);
extern void __VERIFIER_assume(int);
#ifndef CASE
#define CASE 0
#endif
int shared_result;
pthread_mutex_t data_lock=PTHREAD_MUTEX_INITIALIZER, slot_lock=PTHREAD_MUTEX_INITIALIZER;
void unsafe_callback(void) {shared_result++;}
void safe_callback(void) {pthread_mutex_lock(&data_lock); shared_result++; pthread_mutex_unlock(&data_lock);}
void incompatible(int argument) {shared_result=argument;}
#if CASE == 3 || CASE == 10
void (*slot)(void)=unsafe_callback;
#elif CASE == 4
void (*slot)(void)=(void (*)(void))incompatible;
#elif CASE == 8
void (*slot)(void)=0;
#else
void (*slot)(void)=safe_callback;
#endif
void (*other_slot)(void)=unsafe_callback;
void install(void (*value)(void)) {slot=value;}
void install_variadic(void (*value)(void), ...) {slot=value;}
void invoke(void (*value)(void)) {value();}
void* entry(void* unused) {
  pthread_mutex_lock(&slot_lock); void (*saved)(void)=slot; pthread_mutex_unlock(&slot_lock);
#if CASE == 6 || CASE == 7
  invoke(saved);
#if CASE == 7
  saved=other_slot; invoke(saved);
#endif
#elif CASE == 12
  if (saved != safe_callback) return 0;
  saved();
#elif CASE == 9
  if (saved != unsafe_callback) return 0;
  saved();
#else
  saved();
#endif
  return 0;
}
int main(void) {
  pthread_t first,second;
#if CASE == 11
  install(unsafe_callback);
#elif CASE == 13
  install_variadic(unsafe_callback, 19, 23);
#endif
  pthread_create(&first,0,entry,0);
#if CASE == 10
  pthread_create(&second,0,entry,0);
  return 0;
#endif
  pthread_mutex_lock(&slot_lock);
#if CASE == 0 || CASE == 6 || CASE == 9 || CASE == 12
  slot=unsafe_callback;
#elif CASE == 2
  int value=__VERIFIER_nondet_int(); __VERIFIER_assume(value==0);
  if (value==1) slot=unsafe_callback;
#elif CASE == 5
  install(unsafe_callback);
#endif
  pthread_mutex_unlock(&slot_lock);
  pthread_mutex_lock(&data_lock);
  int result=shared_result;
  pthread_mutex_unlock(&data_lock);
  return result;
}
