#include <pthread.h>
#if __SIZEOF_POINTER__ == 4
extern void *malloc(unsigned long long);
#else
extern void *malloc(unsigned int);
#endif
extern void abort(void);
static int *p;
static int *make(void) {
  int *result = malloc(sizeof(int));
  if (!result) abort();
  *result = 1;
  return result;
}
static void *worker(void *arg) { (void)arg; (*p)++; return 0; }
int main(void) { pthread_t t; p = make(); pthread_create(&t, 0, worker, 0); (*p)++; return 0; }
