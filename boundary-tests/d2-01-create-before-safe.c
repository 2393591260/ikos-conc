#include "_boilerplate.h"
int glob, other;
void *t(void *a){ other = glob; return 0; }   /* child READ of glob */
int main(){ pthread_t id;
  glob = 1;                 /* write BEFORE create => HB */
  pthread_create(&id,0,t,0);
  pthread_join(id,0); return 0; }
