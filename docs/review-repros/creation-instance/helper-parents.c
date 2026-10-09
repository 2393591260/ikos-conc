#include <pthread.h>
int payload;
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
void *consumer(void *arg) {
  volatile int observed = payload;
  return 0;
}
void write_payload(void) {
  pthread_mutex_lock(&lock);
  payload++;
  pthread_mutex_unlock(&lock);
}
void *producer(void *arg) {
  pthread_t c;
  write_payload();
  pthread_create(&c, 0, consumer, 0);
  return 0;
}
int main(void) {
  pthread_t a,b;
  pthread_create(&a,0,producer,0);
  pthread_create(&b,0,producer,0);
  pthread_join(a,0);
  pthread_join(b,0);
  return 0;
}
