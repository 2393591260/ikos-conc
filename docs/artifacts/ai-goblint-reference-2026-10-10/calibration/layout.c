#include <pthread.h>
extern void __goblint_check(int);
int main(void) {
  __goblint_check(sizeof(int) == 4);
#ifdef ABI32
  __goblint_check(sizeof(long) == 4);
  __goblint_check(sizeof(void *) == 4);
  __goblint_check(sizeof(pthread_t) == 4);
  __goblint_check(sizeof(pthread_mutex_t) == 24);
#else
  __goblint_check(sizeof(long) == 8);
  __goblint_check(sizeof(void *) == 8);
  __goblint_check(sizeof(pthread_t) == 8);
  __goblint_check(sizeof(pthread_mutex_t) == 40);
#endif
  return 0;
}
