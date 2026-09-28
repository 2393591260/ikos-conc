#include "_boilerplate.h"
int a, b;
void *t(void *x){ a++; return 0; }
int main(){ pthread_t id; pthread_create(&id,0,t,0);
  b++;                       /* different cell, no race */
  pthread_join(id,0); return 0; }
