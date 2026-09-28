#include <pthread.h>
struct s { int a; int b; };
struct s g;
pthread_mutex_t m1 = PTHREAD_MUTEX_INITIALIZER, m2 = PTHREAD_MUTEX_INITIALIZER;
void *t(void *x){ pthread_mutex_lock(&m1); g.a++; pthread_mutex_unlock(&m1); return 0; }
int main(){ pthread_t t1;
  pthread_create(&t1,0,t,0);
  pthread_mutex_lock(&m2); g.b++; pthread_mutex_unlock(&m2);
  pthread_join(t1,0); return 0; }
