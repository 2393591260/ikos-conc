#include "_boilerplate.h"
int glob;
void *t(void *a){ glob++; return 0; }
int main(){ pthread_t id;
  pthread_create(&id,0,t,0);
  glob++;                   /* RACE: parent write AFTER create, no lock */
  pthread_join(id,0); return 0; }
