// Actual pre-create helper allocations: both ABI controls, not benchmark rewrites.
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#ifndef CASE
#define CASE 0
#endif
#if CASE == 13
#define DATUM long long
#else
#define DATUM int
#endif
struct node { DATUM datum; struct node *next; };
static struct node *A, *B;
static int flag;
#if CASE == 20
static int native_cell;
#endif
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned allocation_calls;
static void *raw(size_t n) {
  ++allocation_calls;
#if CASE == 5
  return calloc(1, n);
#elif CASE == 17
  return calloc((size_t)-1, 2);
#else
  return malloc(n);
#endif
}
static void *safe_alloc(size_t n) {
  void *p = raw(n);
  if (!p) abort();
  return p;
}
static struct node *make(int x) {
#if CASE == 7
  struct node local;
  local.datum = x; local.next = NULL;
  return &local;
#else
  struct node *p = safe_alloc(sizeof(*p));
#if CASE != 4 && CASE != 5
  p->datum = x;
#endif
  p->next = NULL;
  return p;
#endif
}
static void *worker(void *arg) {
  (void)arg;
#if CASE == 3 || CASE == 6 || CASE == 18
  flag++;
#else
  pthread_mutex_lock(&lock);
#if CASE == 2
  B->datum++;
#else
  A->datum++;
#endif
  pthread_mutex_unlock(&lock);
#endif
  return NULL;
}
int main(void) {
  pthread_t t;
#if CASE == 8
  pthread_create(&t, NULL, worker, NULL);
#endif
#if CASE == 3 || CASE == 6 || CASE == 18
  A = raw(CASE == 18 ? 0 : sizeof(*A));
#if CASE == 6
  if (A) abort();
  A->datum = 1;
#endif
#else
#if CASE == 15
  for (int i = 0; i < 2; ++i) {
    struct node *p = make(i + 3);
    if (i == 0) A = p; else B = p;
  }
#else
  A = make(3);
  B = make(5);
#endif
#if CASE == 4
  volatile DATUM sink = A->datum;
  (void)sink;
#elif CASE == 9
  free(A);
#elif CASE == 12
  volatile uintptr_t numeric = (uintptr_t)A;
  if (numeric == 0) abort();
#elif CASE == 14
  A[2].datum = 7;
#elif CASE == 19
  A->next = B;
#endif
#endif
#if CASE != 8
  pthread_create(&t, NULL, worker, CASE == 16 ? A : NULL);
#endif
#if CASE == 3
  if (A == NULL) flag++;
#elif CASE == 6 || CASE == 18
  flag++;
#else
#if CASE == 10
  A = B;
#endif
  DATUM *data;
  pthread_mutex_lock(&lock);
  data = &A->datum;
#if CASE != 1
  pthread_mutex_unlock(&lock);
#endif
  *data = 42;
#if CASE == 1
  pthread_mutex_unlock(&lock);
#endif
#endif
#if CASE == 20
  __atomic_store_n(&native_cell, 1, __ATOMIC_SEQ_CST);
#endif
  return 0;
}
