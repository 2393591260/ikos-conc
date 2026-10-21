// Native GNU/Linux headers are intentional: do not replace pthread_t with int.
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>

#if CASE == 0
// join-copy
int value;
void *worker(void *p) {
  value=1;
  return 0;
}
int main(void) {
  pthread_t t;
  pthread_create(&t, 0, worker, 0);
  pthread_t copy=t;
  pthread_join(copy, 0);
  value=2;
  return 0;
}

#elif CASE == 1
// detach-overlap
int value;
void *worker(void *p) {
  value=1;
  return 0;
}
int main(void) {
  pthread_t t;
  pthread_create(&t, 0, worker, 0);
  pthread_detach(t);
  value=2;
  return 0;
}

#elif CASE == 2
// heap-native-layout
struct Box {
  void *pointer;
  long long value;
};
void *worker(void *p) {
  ((struct Box *)p)->value=1;
  return 0;
}
int main(void) {
  struct Box *p=malloc(sizeof(*p));
  if (!p) return 0;
  p->pointer=0;
  p->value=0;
  pthread_t t;
  pthread_create(&t, 0, worker, p);
  p->value=2;
  pthread_join(t, 0);
  free(p);
  return 0;
}

#elif CASE == 3
// heap-overlap
union Data {
  long long whole;
  int words[2];
};
void *worker(void *p) {
  ((union Data *)p)->whole=1;
  return 0;
}
int main(void) {
  union Data *p=malloc(sizeof(*p));
  if (!p) return 0;
  p->whole=0;
  pthread_t t;
  pthread_create(&t, 0, worker, p);
  p->words[1]=2;
  pthread_join(t, 0);
  free(p);
  return 0;
}

#elif CASE == 4
// numeric-truncation
int value;
void *worker(void *p) {
  if ((uintptr_t)p==1) value=1;
  return 0;
}
int main(void) {
  pthread_t t;
  pthread_create(&t, 0, worker,(void *)(uintptr_t)0x100000001ULL);
  value=2;
  pthread_join(t, 0);
  return 0;
}

#elif CASE == 5
// numeric-sign-extension
int value;
void *worker(void *p) {
  if ((uintptr_t)p==UINTPTR_MAX) value=1;
  return 0;
}
int main(void) {
  pthread_t t;
  pthread_create(&t, 0, worker,(void *)(intptr_t)-1);
  value=2;
  pthread_join(t, 0);
  return 0;
}

#elif CASE == 6
// mutex-before-create
int value;
pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
void *worker(void *p) {
  pthread_mutex_lock(&m);
  value=1;
  pthread_mutex_unlock(&m);
  return 0;
}
int main(void) {
  pthread_t t;
  pthread_mutex_lock(&m);
  pthread_create(&t, 0, worker, 0);
  value=2;
  pthread_mutex_unlock(&m);
  pthread_join(t, 0);
  return 0;
}

#elif CASE == 7
// once-common-control
int value;
pthread_once_t once = PTHREAD_ONCE_INIT;
void init(void) {
  value=1;
}
void *worker(void *p) {
  pthread_once(&once, init);
  return 0;
}
int main(void) {
  pthread_t t;
  pthread_once(&once, init);
  pthread_create(&t, 0, worker, 0);
  pthread_once(&once, init);
  pthread_join(t, 0);
  return value;
}

#elif CASE == 8
// once-distinct-controls
int value;
pthread_once_t a = PTHREAD_ONCE_INIT,b = PTHREAD_ONCE_INIT;
void init(void) {
  value=1;
}
void *worker(void *p) {
  pthread_once(&a, init);
  return 0;
}
int main(void) {
  pthread_t t;
  pthread_create(&t, 0, worker, 0);
  pthread_once(&b, init);
  pthread_join(t, 0);
  return 0;
}

#elif CASE == 9
// barrier-ordered
int value;
pthread_barrier_t b;
void *worker(void *p) {
  value=1;
  pthread_barrier_wait(&b);
  return 0;
}
int main(void) {
  pthread_t t;
  if (pthread_barrier_init(&b, 0,2)) return 0;
  pthread_create(&t, 0, worker, 0);
  pthread_barrier_wait(&b);
  value=2;
  pthread_join(t, 0);
  pthread_barrier_destroy(&b);
  return 0;
}

#elif CASE == 10
// barrier-overlap
int value;
pthread_barrier_t b;
void *worker(void *p) {
  pthread_barrier_wait(&b);
  value=1;
  return 0;
}
int main(void) {
  pthread_t t;
  if (pthread_barrier_init(&b, 0,2)) return 0;
  pthread_create(&t, 0, worker, 0);
  pthread_barrier_wait(&b);
  value=2;
  pthread_join(t, 0);
  pthread_barrier_destroy(&b);
  return 0;
}

#elif CASE == 11
// recursive-mutex
int value;
pthread_mutex_t m;
void *worker(void *p) {
  pthread_mutex_lock(&m);
  value=1;
  pthread_mutex_unlock(&m);
  return 0;
}
int main(void) {
  pthread_t t;
  pthread_mutexattr_t a;
  pthread_mutexattr_init(&a);
  pthread_mutexattr_settype(&a,PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&m,&a);
  pthread_mutexattr_destroy(&a);
  pthread_mutex_lock(&m);
  pthread_mutex_lock(&m);
  pthread_create(&t, 0, worker, 0);
  value=2;
  pthread_mutex_unlock(&m);
  pthread_mutex_unlock(&m);
  pthread_join(t, 0);
  pthread_mutex_destroy(&m);
  return 0;
}

#else
#error Unknown CASE
#endif
