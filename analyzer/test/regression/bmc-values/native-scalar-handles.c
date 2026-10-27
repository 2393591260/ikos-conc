// Opaque native storage must not imply Join completion.
#include <pthread.h>
#include <string.h>
#ifndef UNPROTECTED
#define UNPROTECTED 0
#endif
#ifdef WIDE
typedef long long datum_type;
#else
typedef int datum_type;
#endif
pthread_mutex_t guard = PTHREAD_MUTEX_INITIALIZER;
datum_type data;
void *escaped;
extern void unknown(void *);
/* EXTRA_DECLARATIONS */
void *worker(void *unused) {
  (void)unused;
#if !UNPROTECTED
  pthread_mutex_lock(&guard);
#endif
  data++;
#if !UNPROTECTED
  pthread_mutex_unlock(&guard);
#endif
  return NULL;
}
int main(void) {
  pthread_t first, second;
  /* PREFIX */
  pthread_create(&first, NULL, worker, NULL);
  pthread_create(&second, NULL, worker, NULL);
  /* BETWEEN */
  pthread_join(first, NULL);
  pthread_join(second, NULL);
  /* SUFFIX */
  return 0;
}
