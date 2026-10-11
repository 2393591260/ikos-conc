#include <pthread.h>
#ifndef CASE
#define CASE 0
#endif
#if CASE == 1
struct other;
struct node { struct other *next; int value; };
struct other { struct node *next; };
#elif CASE == 2
struct leaf { int value; };
struct node { struct leaf first, second; };
#else
struct node { struct node *next; int value; };
#endif
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static void *worker(void *p) {
#if CASE == 3
  pthread_mutex_lock(&mutex);
#endif
#if CASE == 2
  ((struct node *)p)->first.value = 1;
#else
  ((struct node *)p)->value = 1;
#endif
#if CASE == 3
  pthread_mutex_unlock(&mutex);
#endif
  return 0;
}
int main(void) {
  struct node value;
#if CASE == 2
  value.first.value = 0; value.second.value = 0;
#else
  value.next = 0; value.value = 0;
#endif
  pthread_t a, b;
  pthread_create(&a, 0, worker, &value);
  pthread_create(&b, 0, worker, &value);
  pthread_join(a, 0); pthread_join(b, 0);
  return 0;
}
