#include <pthread.h>
#include <stdatomic.h>
int data;
atomic_int flag;
void *t(void *x){ data = 42; atomic_store_explicit(&flag, 1, memory_order_release); return 0; }
int main(){ pthread_t t1;
  pthread_create(&t1,0,t,0);
  while (!atomic_load_explicit(&flag, memory_order_acquire)) {}
  int r = data;
  pthread_join(t1,0); return r; }
