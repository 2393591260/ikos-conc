#include <pthread.h>

volatile int data;
void* writer(void* arg) {
  data = 7;
  data = 9;
  return 0;
}
int main(void) {
  pthread_t thread;
  pthread_create(&thread, 0, writer, 0);
  return data;
}
