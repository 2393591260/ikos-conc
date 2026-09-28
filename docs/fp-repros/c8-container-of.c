#include <pthread.h>
#include <stddef.h>
struct s { pthread_mutex_t m; int data; };
struct s g;
#define container_of(ptr, type, member) ((type*)((char*)(ptr) - offsetof(type, member)))
void *t(void *x){
  pthread_mutex_t *pm = &g.m;
  pthread_mutex_lock(pm);
  struct s *ps = container_of(pm, struct s, m);
  ps->data++;
  pthread_mutex_unlock(pm);
  return 0; }
int main(){ pthread_t t1;
  pthread_create(&t1,0,t,0);
  pthread_mutex_lock(&g.m); g.data++; pthread_mutex_unlock(&g.m);
  pthread_join(t1,0); return 0; }
