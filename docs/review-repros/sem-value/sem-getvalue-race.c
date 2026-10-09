#include <pthread.h>
#include <semaphore.h>
sem_t semaphore;
int output;
void *get(void *arg) { sem_getvalue(&semaphore, &output); return 0; }
void *write(void *arg) { output = 1; return 0; }
int main(void) {
  sem_init(&semaphore, 0, 0);
  pthread_t a, b;
  pthread_create(&a, 0, get, 0);
  pthread_create(&b, 0, write, 0);
  return 0;
}
