#include "_boilerplate.h"
int glob, ready = 0;
pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t c = PTHREAD_COND_INITIALIZER;
void *t(void *x){ pthread_mutex_lock(&m); glob=1; ready=1; pthread_cond_signal(&c); pthread_mutex_unlock(&m); return 0; }
int main(){ pthread_t id; pthread_create(&id,0,t,0);
  pthread_mutex_lock(&m); while(!ready) pthread_cond_wait(&c,&m); int y=glob; (void)y; pthread_mutex_unlock(&m);
  pthread_join(id,0); return 0; }
