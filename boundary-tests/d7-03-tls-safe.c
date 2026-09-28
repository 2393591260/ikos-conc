#include "_boilerplate.h"
_Thread_local int tl;
void *t(void *x){ tl++; return 0; }   /* per-thread copy, no race */
int main(){ pthread_t id; pthread_create(&id,0,t,0);
  tl++;
  pthread_join(id,0); return 0; }
