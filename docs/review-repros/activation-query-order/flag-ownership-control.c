#include <pthread.h>
extern int __VERIFIER_nondet_int(void);
extern void __VERIFIER_assume(int);
extern void __VERIFIER_atomic_begin(void);
extern void __VERIFIER_atomic_end(void);
int held,data;
void enter(void) {
  __VERIFIER_atomic_begin();
  __VERIFIER_assume(held == 0);
  held = 1;
  __VERIFIER_atomic_end();
}
void leave(void) {
  __VERIFIER_atomic_begin();
  __VERIFIER_assume(held == 1);
  held = 0;
  __VERIFIER_atomic_end();
}
void *first(void *p) { enter(); data=1; leave(); return 0; }
void *second(void *p) { enter(); data=2; leave(); return 0; }
int main(void) {
  pthread_t t;
  while (__VERIFIER_nondet_int()) {
    if (__VERIFIER_nondet_int()) pthread_create(&t, 0, first, 0);
    else pthread_create(&t, 0, second, 0);
  }
  return 0;
}
