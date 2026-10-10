#include <assert.h>
#include <pthread.h>
pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
void *reader(void *unused) {
  assert(pthread_mutex_lock(&mutex)==0);
  assert(mutex.__data.__lock!=0);
  assert(pthread_mutex_unlock(&mutex)==0);
  return 0;
}
int main(void){
  pthread_t a;
  assert(pthread_create(&a,0,reader,0)==0);
  assert(pthread_join(a,0)==0);
  return 0;
}
