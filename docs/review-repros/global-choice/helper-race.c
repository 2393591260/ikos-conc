#include <pthread.h>
extern int __VERIFIER_nondet_int(void);
int left, right;
void *writer(void *arg) {
  right = 9;
  return 0;
}
int *choose(int flag) { return flag ? &left : &right; }
int main(void) {
  pthread_t t;
  int *destination = choose(__VERIFIER_nondet_int());
  pthread_create(&t, 0, writer, 0);
  *destination = 7;
  pthread_join(t, 0);
  return 0;
}
