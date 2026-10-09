#include <pthread.h>
/* No library summary: an external implementation may write the output. */
extern void vendor_pthread_mutex_copy(int *);
int output;
void *getter(void *unused) { vendor_pthread_mutex_copy(&output); return 0; }
void *writer(void *unused) { output = 1; return 0; }
int main(void) {
  pthread_t a, b;
  pthread_create(&a, 0, getter, 0);
  pthread_create(&b, 0, writer, 0);
  return 0;
}
