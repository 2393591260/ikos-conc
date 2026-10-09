#include <pthread.h>

int data;
void* worker(void* arg) {
  data = (int)(long)arg;
  return 0;
}

int main(void) {
  pthread_t threads[2];
  for (int i = 0; i < 2; ++i)
    pthread_create(&threads[i], 0, worker, (void*)(long)(i + 1));
  return 0;
}
