#include "_boilerplate.h"
int glob;
void *t(void *a){ glob++; return 0; }   /* single instance, no cross-thread pair */
int main(){ pthread_t id; pthread_create(&id,0,t,0); pthread_join(id,0); return 0; }
