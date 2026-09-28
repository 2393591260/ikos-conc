#include "_boilerplate.h"
int m[16];
int idx;
void *t(void *x) { m[idx] = 1; return 0; }
void *u(void *x) { m[idx] = 2; return 0; }
int main(){ pthread_t t1, t2;
  idx = __VERIFIER_nondet_int();
  pthread_create(&t1,0,t,0);
  pthread_create(&t2,0,u,0);
  pthread_join(t1,0); pthread_join(t2,0); return 0; }
