#include <pthread.h>
#include <stdatomic.h>
int data;
atomic_int lk = 0;
void *t(void *x){
  while (atomic_exchange_explicit(&lk, 1, memory_order_acquire)) {}
  data++;
  atomic_store_explicit(&lk, 0, memory_order_release);
  return 0; }
int main(){ pthread_t t1;
  pthread_create(&t1,0,t,0);
  while (atomic_exchange_explicit(&lk, 1, memory_order_acquire)) {}
  data++;
  atomic_store_explicit(&lk, 0, memory_order_release);
  pthread_join(t1,0); return 0; }
