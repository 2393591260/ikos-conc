#include <pthread.h>
extern int __VERIFIER_nondet_int(void);
int output;
pthread_mutexattr_t attr;
int harmless(const pthread_mutexattr_t *a, int *out) { return 0; }
void *getter(void *unused) {
  int (*get)(const pthread_mutexattr_t *, int *) =
      __VERIFIER_nondet_int() ? pthread_mutexattr_gettype : harmless;
  get(&attr, &output);
  return 0;
}
void *writer(void *unused) { output = 1; return 0; }
int main(void) {
  pthread_t a, b;
  pthread_mutexattr_init(&attr);
  pthread_create(&a, 0, getter, 0);
  pthread_create(&b, 0, writer, 0);
  return 0;
}
