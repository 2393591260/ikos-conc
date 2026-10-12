#define _GNU_SOURCE
#include <pthread.h>
#include <assert.h>
#include <errno.h>
#include <stdio.h>
static void *other_owner(void *mutex) {
  assert(pthread_mutex_unlock(mutex) == EPERM);
  return 0;
}
int main(void) {
  pthread_mutex_t static_mutex = PTHREAD_ERRORCHECK_MUTEX_INITIALIZER_NP;
  assert(pthread_mutex_lock(&static_mutex) == 0);
  assert(pthread_mutex_lock(&static_mutex) == EDEADLK);
  assert(pthread_mutex_unlock(&static_mutex) == 0);
  assert(pthread_mutex_trylock(&static_mutex) == 0);
  assert(pthread_mutex_unlock(&static_mutex) == 0);
  assert(pthread_mutex_destroy(&static_mutex) == 0);
  _Static_assert(PTHREAD_MUTEX_ERRORCHECK == 2, "glibc errorcheck code");
  _Static_assert(sizeof(pthread_mutexattr_t) == 4, "glibc attribute size");
  _Static_assert(sizeof(pthread_mutex_t) == (sizeof(void *) == 4 ? 24 : 40), "glibc mutex size");
  pthread_mutexattr_t a;
  pthread_mutex_t m;
  assert(pthread_mutexattr_init(&a) == 0);
  assert(pthread_mutexattr_settype(&a, PTHREAD_MUTEX_ERRORCHECK) == 0);
  assert(pthread_mutex_init(&m, &a) == 0);
  assert(pthread_mutexattr_destroy(&a) == 0);
  assert(pthread_mutex_lock(&m) == 0);
  pthread_t thread;
  assert(pthread_create(&thread, 0, other_owner, &m) == 0);
  assert(pthread_join(thread, 0) == 0);
  assert(pthread_mutex_unlock(&m) == 0);
  assert(pthread_mutex_unlock(&m) == EPERM);
  assert(pthread_mutex_destroy(&m) == 0);
  printf("mutex=%zu attr=%zu errorcheck=%d\n", sizeof(m), sizeof(a), PTHREAD_MUTEX_ERRORCHECK);
}
