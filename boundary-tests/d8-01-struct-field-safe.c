#include "_boilerplate.h"
struct S { pthread_mutex_t m; int x; } s = { PTHREAD_MUTEX_INITIALIZER, 0 };
void *t(void *a){ pthread_mutex_lock(&s.m); s.x++; pthread_mutex_unlock(&s.m); return 0; }
int main(){ pthread_t id; pthread_create(&id,0,t,0);
  pthread_mutex_lock(&s.m); s.x++; pthread_mutex_unlock(&s.m);
  pthread_join(id,0); return 0; }
