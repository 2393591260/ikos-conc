#include "_boilerplate.h"
int glob, other;
void *t(void *a){ glob = 1; return 0; }
int main(){ pthread_t id;
  pthread_create(&id,0,t,0);
  pthread_join(id,0);
  other = glob;             /* READ after join => HB */
  return 0; }
