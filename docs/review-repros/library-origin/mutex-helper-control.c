#include <pthread.h>
pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
int payload;
static int lock_state(void) { return mutex.__data.__lock; }
void *reader(void *unused) {
  pthread_mutex_lock(&mutex);
  int state=lock_state();
  pthread_mutex_unlock(&mutex);
  if(state==0) payload=1;
  return 0;
}
void *writer(void *unused) {payload=2; return 0;}
int main(void){pthread_t a,b;pthread_create(&a,0,reader,0);pthread_create(&b,0,writer,0);return 0;}
