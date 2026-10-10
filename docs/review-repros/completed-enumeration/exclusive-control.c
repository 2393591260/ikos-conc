#include <pthread.h>
#include <stdio.h>
extern int __VERIFIER_nondet_int(void);
int data, selector;
int value(void) {
  for (int i = 0; i < 12; ++i) {
    if (__VERIFIER_nondet_int()) puts("left");
    else puts("right");
  }
  return 1;
}
void *a(void *arg) { int x=value(); if(selector==0) data=x; return 0; }
void *b(void *arg) { int x=value(); if(selector!=0) data=x; return 0; }
int main(void) {
  pthread_t x,y;
  selector=__VERIFIER_nondet_int();
  pthread_create(&x,0,a,0);
  pthread_create(&y,0,b,0);
  pthread_join(x,0);
  pthread_join(y,0);
  return 0;
}
