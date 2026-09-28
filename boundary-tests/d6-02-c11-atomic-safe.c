#include "_boilerplate.h"
#include <stdatomic.h>
_Atomic int glob;
void *t(void *x){ atomic_fetch_add(&glob, 1); return 0; }
int main(){ pthread_t id; pthread_create(&id,0,t,0);
  atomic_fetch_add(&glob, 1);
  pthread_join(id,0); return 0; }
