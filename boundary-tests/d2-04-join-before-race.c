#include "_boilerplate.h"
int glob, other;
void *t(void *a){ glob = 1; return 0; }
int main(){ pthread_t id;
  pthread_create(&id,0,t,0);
  other = glob;             /* READ before join => RACE with child write */
  pthread_join(id,0); return 0; }
