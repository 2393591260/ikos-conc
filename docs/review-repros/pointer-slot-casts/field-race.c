#include <pthread.h>
#include <stdlib.h>
struct Payload { int flag; int spare; };
struct Payload *p;
int data;
void *worker(void *arg) {
  if(p->flag==4) data=1;
  return 0;
}
int main(void) {
  pthread_t t;
  p=malloc(sizeof(*p));
  p->flag=4;
  pthread_create(&t,0,worker,0);
  data=2;
  pthread_join(t,0);
  return 0;
}
