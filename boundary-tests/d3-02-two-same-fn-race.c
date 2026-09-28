#include "_boilerplate.h"
int glob;
void *t(void *a){ glob++; return 0; }   /* two instances of SAME fn => non-unique */
int main(){ pthread_t a,b;
  pthread_create(&a,0,t,0); pthread_create(&b,0,t,0);
  pthread_join(a,0); pthread_join(b,0); return 0; }
