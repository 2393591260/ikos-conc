#include "_boilerplate.h"
int glob;
void *t(void *x){ int *p = &glob; *p += 1; return 0; }
int main(){ pthread_t id; pthread_create(&id,0,t,0);
  glob += 1;                 /* RACE: same cell via alias */
  pthread_join(id,0); return 0; }
