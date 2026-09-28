#include "_boilerplate.h"
void *t(void *arg){ int *p = (int*)arg; *p += 1; return 0; }
int main(){ int local = 0; pthread_t id;
  pthread_create(&id,0,t,&local);  /* &local escapes via spawn arg */
  local += 1;              /* RACE on escaped stack local */
  pthread_join(id,0); return 0; }
