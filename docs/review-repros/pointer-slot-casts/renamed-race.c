#include <pthread.h>
#include <stdlib.h>
struct Record { int flag; int spare; };
struct Record *published;
int shared_count;
void *reader(void *arg) {
  if(published->flag==4) shared_count=1;
  return 0;
}
int main(void) {
  pthread_t t;
  published=malloc(sizeof(*published));
  published->flag=4;
  pthread_create(&t,0,reader,0);
  shared_count=2;
  pthread_join(t,0);
  return 0;
}
