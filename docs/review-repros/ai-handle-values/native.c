#include <pthread.h>
#include <assert.h>
#include <stdio.h>
static void *worker(void *p) {(void)p;return 0;}
int main(void) {
  pthread_t t;
  assert(pthread_create(&t,0,worker,0)==0);
  printf("pthread_t=%lu, differs from numeric 1: %d\n",(unsigned long)t,t!=(pthread_t)1);
  pthread_t self=pthread_self();
  printf("pthread_self=%lu, differs from main token: %d\n",(unsigned long)self,self!=(pthread_t)0xA7E0AD1DULL);
  assert(t!=(pthread_t)1);
  assert(self!=(pthread_t)0xA7E0AD1DULL);
  assert(pthread_join(t,0)==0);
}
