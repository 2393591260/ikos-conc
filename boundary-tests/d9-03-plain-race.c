#include "_boilerplate.h"
int glob;
void *t(void *x){ glob = 1; return 0; }
int main(){ pthread_t id;
  pthread_create(&id,0,t,0);
  glob = 2;
  pthread_join(id,0); return 0; }
