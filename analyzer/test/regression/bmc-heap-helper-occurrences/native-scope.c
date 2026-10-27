// New helper heap and native SC remain separate execution profiles.
#include <pthread.h>
#include <stdlib.h>
#ifndef CASE
#define CASE 0
#endif
static int *p, *other;
static int cell;
static int *make(void) {
  int *value = malloc(sizeof(*value));
  if (!value) abort();
  *value = 0;
  return value;
}
static void *worker(void *arg) { (void)arg; (*p)++; return NULL; }
int main(void) {
  pthread_t t;
  p = make();
  other = make();
  if (!other) abort();
  pthread_create(&t, NULL, worker, NULL);
#if CASE == 0
  __atomic_store_n(&cell, 1, __ATOMIC_SEQ_CST);
#elif CASE == 1
  volatile int value = __atomic_load_n(&cell, __ATOMIC_SEQ_CST);
  (void)value;
#elif CASE == 2
  __atomic_fetch_add(&cell, 1, __ATOMIC_SEQ_CST);
#else
  int expected = 0;
  __atomic_compare_exchange_n(&cell, &expected, 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
#endif
  (*p)++;
  return 0;
}
