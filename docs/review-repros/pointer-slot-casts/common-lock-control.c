#include <pthread.h>
#include <stdlib.h>
struct Payload { int flag; int spare; };
struct Payload *p;
int data;
pthread_mutex_t m=PTHREAD_MUTEX_INITIALIZER;
void *worker(void *arg) {
  pthread_mutex_lock(&m);
  if(p->flag==4) data=1;
  pthread_mutex_unlock(&m);
  return 0;
}
int main(void) {
  pthread_t t;
  p=malloc(sizeof(*p));
  p->flag=4;
  pthread_create(&t,0,worker,0);
  pthread_mutex_lock(&m);
  data=2;
  pthread_mutex_unlock(&m);
  pthread_join(t,0);
  return 0;
}
