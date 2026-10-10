#include <pthread.h>
extern int __VERIFIER_nondet_int(void);
static int payload;
static struct { pthread_once_t left, right; } pair = {PTHREAD_ONCE_INIT,PTHREAD_ONCE_INIT};
static pthread_once_t array[2] = {PTHREAD_ONCE_INIT,PTHREAD_ONCE_INIT};
static _Thread_local pthread_once_t local = PTHREAD_ONCE_INIT;
#if CASE == 7
extern void callback(void);
#else
static void callback(void) { payload = 1; }
#endif
static void other(void) { payload = 2; }
#if CASE == 6
static void (*indirect)(void) = callback;
#endif
static void *worker(void *arg) {
  (void)arg;
#if CASE == 0 || CASE == 1 || CASE == 8 || CASE == 10 || CASE == 11
  pthread_once(&pair.left, callback);
#elif CASE == 2 || CASE == 3
  pthread_once(&array[0], callback);
#elif CASE == 4
  pthread_once(&array[__VERIFIER_nondet_int() ? 0 : 1], callback);
#elif CASE == 5
  pthread_once(&local, callback);
#elif CASE == 6
  pthread_once(&pair.left, indirect);
#elif CASE == 7
  pthread_once(&pair.left, callback);
#endif
  return 0;
}
static void *other_worker(void *arg) {
  (void)arg;
#if CASE == 0 || CASE == 8 || CASE == 10
  pthread_once(&pair.right, callback);
#elif CASE == 2 || CASE == 4
  pthread_once(&array[1], callback);
#elif CASE == 5
  pthread_once(&local, callback);
#else
  return worker(0);
#endif
  return 0;
}
// Different syntactic paths to the same control must retain one identity.
static void *helper(void *arg) {
  (void)arg;
#if CASE == 10 || CASE == 11
  pthread_once(&pair.left, other);
#elif CASE == 9
  pthread_once((pthread_once_t*)((char*)&pair + __builtin_offsetof(__typeof__(pair),left)), callback);
#else
  pthread_once(&pair.left, callback);
#endif
  return 0;
}
int main(void) {
  pthread_t a,b,c;
#if CASE == 10 || CASE == 11
  // The first control may execute either callback; the second can still
  // execute callback concurrently with the first choosing that callback.
  pthread_create(&a,0,helper,0);
  pthread_create(&b,0,worker,0);
  pthread_create(&c,0,other_worker,0);
  pthread_join(c,0);
#elif CASE == 0 || CASE == 2 || CASE == 5
  pthread_create(&a,0,worker,(void*)0); pthread_create(&b,0,other_worker,(void*)1);
#elif CASE == 1 || CASE == 3
  pthread_create(&a,0,worker,(void*)0); pthread_create(&b,0,worker,(void*)0);
#elif CASE == 4
  pthread_create(&a,0,worker,(void*)(long)(__VERIFIER_nondet_int() ? 0 : 1));
  pthread_create(&b,0,other_worker,(void*)1);
#elif CASE == 6 || CASE == 7
  pthread_create(&a,0,worker,0);
  payload=3;
  pthread_create(&b,0,helper,0);
#elif CASE == 8
  pthread_create(&a,0,worker,(void*)0); pthread_join(a,0);
  pthread_create(&b,0,other_worker,(void*)1); pthread_join(b,0); return 0;
#else
  pthread_create(&a,0,helper,0); pthread_create(&b,0,helper,0);
#endif
  pthread_join(a,0); pthread_join(b,0); return 0;
}
