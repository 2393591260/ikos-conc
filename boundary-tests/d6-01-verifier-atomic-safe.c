#include "_boilerplate.h"
int glob;
void *t(void *x){ __VERIFIER_atomic_begin(); glob++; __VERIFIER_atomic_end(); return 0; }
int main(){ pthread_t id; pthread_create(&id,0,t,0);
  __VERIFIER_atomic_begin(); glob++; __VERIFIER_atomic_end();
  pthread_join(id,0); return 0; }
