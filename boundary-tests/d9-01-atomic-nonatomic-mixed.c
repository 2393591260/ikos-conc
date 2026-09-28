#include "_boilerplate.h"
#include <stdatomic.h>
_Atomic int glob;
int *p;
void *t(void *x){ atomic_store(&glob, 1); return 0; }
void *u(void *x){ *p = 0; return 0; }
int main(){ pthread_t id1, id2;
  p = (int*)&glob;
  pthread_create(&id1,0,t,0);
  pthread_create(&id2,0,u,0);
  pthread_join(id1,0); pthread_join(id2,0); return 0; }
