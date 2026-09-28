#include "_boilerplate.h"
int *gp;                     /* global pointer */
void *t(void *x){ int *p = gp; *p += 1; return 0; }
int main(){ int local = 0; pthread_t id;
  gp = &local;              /* publish stack local via GLOBAL pointer */
  pthread_create(&id,0,t,0);
  local += 1;              /* RACE (theory): same escaped local */
  pthread_join(id,0); return 0; }
