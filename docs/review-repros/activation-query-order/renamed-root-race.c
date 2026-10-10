#include <pthread.h>
extern int __VERIFIER_nondet_int(void);
extern void __VERIFIER_assume(int);
extern void __VERIFIER_atomic_begin(void);
extern void __VERIFIER_atomic_end(void);
int ownership,shared_number;
void enter(void) {
  __VERIFIER_atomic_begin();
  __VERIFIER_assume(ownership == 0);
  ownership = 1;
  __VERIFIER_atomic_end();
}
void leave(void) {
  __VERIFIER_atomic_begin();
  __VERIFIER_assume(ownership == 1);
  ownership = 0;
  __VERIFIER_atomic_end();
}
void *changed_worker(void *p) { enter(); leave(); shared_number=1; return 0; }
void *alternate_worker(void *p) { enter(); shared_number=2; leave(); return 0; }
int main(void) {
  pthread_t t;
  while (__VERIFIER_nondet_int()) {
    if (__VERIFIER_nondet_int()) pthread_create(&t, 0, changed_worker, 0);
    else pthread_create(&t, 0, alternate_worker, 0);
  }
  return 0;
}
