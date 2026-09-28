#include "_boilerplate.h"
int glob;
void *t(void *a){ int i; for(i=0;i<100;i++){ } glob++; return 0; } /* local counter => loop exits */
int main(){ pthread_t id; pthread_create(&id,0,t,0); pthread_join(id,0); return 0; }
