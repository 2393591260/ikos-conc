#include <pthread.h>
#include <stdio.h>
#ifndef CASE
#define CASE 0
#endif
static int data;
static void *worker(void *p) {(void)p;data=1;return 0;}
int main(void) {
  pthread_t t; int count=0; char buffer[4];
  pthread_create(&t,0,worker,0);
#if CASE == 0
  printf("x%n",&count);
#elif CASE == 1
  sprintf(buffer,"x%n",&count);
#elif CASE == 2
  snprintf(0,0,"x%n",&count);
#else
  snprintf(buffer,sizeof buffer,"x%n",&count);
#endif
  if(count==1) data=2;
  pthread_join(t,0);
  return 0;
}
