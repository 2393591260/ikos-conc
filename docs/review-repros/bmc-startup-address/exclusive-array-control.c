#include <pthread.h>
extern int __VERIFIER_nondet_int(void);
int choices[3], data;
void *first(void *arg) {
  if (choices[2] == 0) data = 1;
  return 0;
}
void *second(void *arg) {
  if (choices[2] != 0) data = 2;
  return 0;
}
int main(void) {
  for (int i = 0; i < 3; ++i) choices[i] = __VERIFIER_nondet_int();
  pthread_t a, b;
  pthread_create(&a, 0, first, 0);
  pthread_create(&b, 0, second, 0);
  return 0;
}
