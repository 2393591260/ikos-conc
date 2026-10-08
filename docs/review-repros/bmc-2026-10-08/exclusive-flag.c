#include <pthread.h>
extern int __VERIFIER_nondet_int(void);
int selector;
int x;
void* a(void* arg) {
  if (selector == 1) x = 1;
  return 0;
}
void* b(void* arg) {
  if (selector == 2) x = 2;
  return 0;
}
int main(void) {
  pthread_t ta, tb;
  selector = __VERIFIER_nondet_int();
  pthread_create(&ta, 0, a, 0);
  pthread_create(&tb, 0, b, 0);
  pthread_join(ta, 0);
  pthread_join(tb, 0);
  return 0;
}
