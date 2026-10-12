#include <pthread.h>
#include <string.h>
#include <stddef.h>
#include <semaphore.h>
#include <stdio.h>
#ifdef NATIVE
#include <assert.h>
#include <errno.h>
#include <stddef.h>
int main(void) {
  _Static_assert(sizeof(pthread_mutexattr_t) == 4, "glibc attribute layout");
  _Static_assert(sizeof(pthread_mutex_t) == (sizeof(void *) == 4 ? 24 : 40), "glibc mutex layout");
  _Static_assert(PTHREAD_MUTEX_NORMAL == 0 && PTHREAD_MUTEX_ERRORCHECK == 2, "glibc kinds");
  pthread_mutexattr_t native_attr;
  pthread_mutex_t native_mutex;
  int kind, robust, shared, protocol;
  assert(pthread_mutexattr_init(&native_attr) == 0);
  assert(pthread_mutexattr_settype(&native_attr, PTHREAD_MUTEX_ERRORCHECK) == 0);
  assert(pthread_mutexattr_gettype(&native_attr, &kind) == 0 && kind == 2);
  assert(pthread_mutexattr_getrobust(&native_attr, &robust) == 0 && robust == PTHREAD_MUTEX_STALLED);
  assert(pthread_mutexattr_getpshared(&native_attr, &shared) == 0 && shared == PTHREAD_PROCESS_PRIVATE);
  assert(pthread_mutexattr_getprotocol(&native_attr, &protocol) == 0 && protocol == PTHREAD_PRIO_NONE);
  assert(pthread_mutex_init(&native_mutex, &native_attr) == 0);
  assert(pthread_mutexattr_destroy(&native_attr) == 0);
  assert(pthread_mutex_lock(&native_mutex) == 0);
  assert(pthread_mutex_lock(&native_mutex) == EDEADLK);
  assert(pthread_mutex_unlock(&native_mutex) == 0);
  assert(pthread_mutex_destroy(&native_mutex) == 0);
  return 0;
}
#else
#ifndef CASE
#define CASE 0
#endif
extern int __VERIFIER_nondet_int(void);
extern void fake_mutexattr_settype(pthread_mutexattr_t *, int);
static pthread_mutexattr_t attr;
static pthread_mutex_t mutex, other;
static pthread_mutex_t common = PTHREAD_MUTEX_INITIALIZER;
static int data;
#if CASE == 28 || CASE == 37
static struct { int padding; pthread_mutex_t lock; } embedded = {0, PTHREAD_MUTEX_INITIALIZER};
#define runtime_mutex embedded.lock
#elif CASE == 29 || CASE == 38
static pthread_mutex_t array[2] = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_MUTEX_INITIALIZER};
#define runtime_mutex array[1]
#elif CASE == 30
static pthread_mutex_t *published_mutex;
#elif CASE == 31
static pthread_spinlock_t spin;
#elif CASE == 32
static pthread_rwlock_t rwlock = PTHREAD_RWLOCK_INITIALIZER;
#elif CASE == 33
static pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
#elif CASE == 34
static pthread_once_t once = PTHREAD_ONCE_INIT;
static void callback(void) {}
#elif CASE == 35
static sem_t semaphore;
#elif CASE == 36
extern FILE _IO_2_1_stdout_;
#else
#define runtime_mutex common
#endif
#if CASE == 23
static struct { pthread_mutex_t *lock; } published;
#elif CASE == 24
static int *volatile address = &data;
#elif CASE == 25
static void before_initialization(void) {
  pthread_mutex_lock(&common);
  pthread_mutex_unlock(&common);
}
#elif CASE == 26
__attribute__((constructor)) static void constructor(void) { data = 0; }
#endif

static void initialize(void) {
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
  pthread_mutex_init(&mutex, &attr);
}

static void *worker(void *arg) {
  (void)arg;
#if CASE == 27 || CASE == 28 || CASE == 29
  pthread_mutex_lock(&runtime_mutex);
  pthread_mutex_unlock(&runtime_mutex);
  return 0;
#elif CASE == 30
  pthread_mutex_lock(published_mutex);
  pthread_mutex_unlock(published_mutex);
  return 0;
#elif CASE == 31
  pthread_spin_lock(&spin);
  pthread_spin_unlock(&spin);
  return 0;
#elif CASE == 32
  pthread_rwlock_wrlock(&rwlock);
  pthread_rwlock_unlock(&rwlock);
  return 0;
#elif CASE == 33
  pthread_cond_signal(&condition);
  return 0;
#elif CASE == 34
  pthread_once(&once, callback);
  return 0;
#elif CASE == 35
  sem_post(&semaphore);
  return 0;
#elif CASE == 36
  puts("opaque library storage");
  return 0;
#elif CASE == 37 || CASE == 38
  pthread_mutex_lock(&runtime_mutex);
#elif CASE == 23
  pthread_mutex_t *chosen = published.lock;
  pthread_mutex_lock(chosen);
#elif CASE == 4 || CASE == 11
  pthread_mutex_lock(&common);
#else
  pthread_mutex_lock(&mutex);
#endif
#if CASE == 24
  *address = 1;
#else
  data = 1;
#endif
#if CASE == 37 || CASE == 38
  pthread_mutex_unlock(&runtime_mutex);
#elif CASE == 23
  pthread_mutex_unlock(chosen);
#elif CASE == 4 || CASE == 11
  pthread_mutex_unlock(&common);
#else
  pthread_mutex_unlock(&mutex);
#endif
  return 0;
}

int main(void) {
  pthread_t thread;
#if CASE == 25
  before_initialization();
#endif
#if CASE == 6
  pthread_mutexattr_t attr;
#elif CASE == 13
  _Alignas(pthread_mutexattr_t) unsigned char storage[sizeof(pthread_mutexattr_t)];
#define attr (*(pthread_mutexattr_t *)storage)
#endif
#if CASE == 7
  initialize();
#else
#if CASE == 11
  pthread_create(&thread, 0, worker, 0);
  pthread_join(thread, 0);
#elif CASE == 12
  int bound = __VERIFIER_nondet_int();
  for (int i = 0; i < bound; ++i) {
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
    pthread_mutex_init(&mutex, &attr);
    pthread_mutex_destroy(&mutex);
    pthread_mutexattr_destroy(&attr);
  }
#endif
  pthread_mutexattr_init(&attr);
#if CASE == 14
  fake_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
#elif CASE == 16
  pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
#elif CASE == 17
  pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT);
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
#elif CASE == 19
  /* Intact default NORMAL attributes also initialize successfully. */
#elif CASE == 21
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_NORMAL);
#else
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
#endif
#if CASE == 8
  attr.__size[sizeof(attr) - 1] = 0; /* raw storage invalidates the certificate */
#endif
  pthread_mutex_init(&mutex, &attr);
#if CASE == 3 || CASE == 4 || CASE == 22
  pthread_mutex_init(&other, &attr);
#elif CASE == 10
  pthread_mutex_destroy(&mutex);
  pthread_mutex_init(&mutex, &attr);
#elif CASE == 15
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
#elif CASE == 20
  pthread_mutexattr_destroy(&attr);
#endif
#endif
#if CASE == 22
  pthread_mutex_t *selected = __VERIFIER_nondet_int() ? &mutex : &other;
  ((unsigned char *)selected)[0] = 0; /* mutation through a multi-definition alias */
#elif CASE == 23
  published.lock = &mutex;
#elif CASE == 18
  pthread_mutexattr_t copy;
  memcpy(&copy, &attr, sizeof attr); /* representation transport is not covered */
  if (copy.__size[0] == 123) data = 0;
#endif
#if CASE == 9
  int observed = mutex.__data.__kind;
  if (observed == 1234) data = 0; /* raw runtime-storage read */
#endif
#if CASE == 30
  pthread_mutex_t local_mutex = PTHREAD_MUTEX_INITIALIZER;
  published_mutex = &local_mutex;
#elif CASE == 31
  pthread_spin_init(&spin, PTHREAD_PROCESS_PRIVATE);
#elif CASE == 35
  sem_init(&semaphore, 0, 0);
#endif
  pthread_create(&thread, 0, worker, 0);
#if CASE == 27 || CASE == 28 || CASE == 29
  /* Legal object-representation read conflicts with glibc's owner write. */
  volatile unsigned char observed = ((volatile unsigned char *)&runtime_mutex)
      [offsetof(pthread_mutex_t, __data.__owner)];
  (void)observed;
#elif CASE == 30
  volatile unsigned char observed = ((volatile unsigned char *)&local_mutex)
      [offsetof(pthread_mutex_t, __data.__owner)];
  (void)observed;
#elif CASE == 31
  volatile unsigned char observed = *(volatile unsigned char *)&spin;
  (void)observed;
#elif CASE == 32
  volatile unsigned char observed = *(volatile unsigned char *)&rwlock;
  (void)observed;
#elif CASE == 33
  volatile unsigned char observed = *(volatile unsigned char *)&condition;
  (void)observed;
#elif CASE == 34
  volatile unsigned char observed = *(volatile unsigned char *)&once;
  (void)observed;
#elif CASE == 35
  volatile unsigned char observed = *(volatile unsigned char *)&semaphore;
  (void)observed;
#elif CASE == 36
  volatile unsigned char observed = *(volatile unsigned char *)&_IO_2_1_stdout_;
  (void)observed;
#elif CASE == 37 || CASE == 38
  pthread_mutex_lock(&runtime_mutex);
  data = 2;
  pthread_mutex_unlock(&runtime_mutex);
#elif CASE == 1
  data = 2;
#elif CASE == 2
  pthread_mutex_trylock(&mutex); /* unchecked failure gives no MUST ownership */
  data = 2;
  pthread_mutex_unlock(&mutex);
#elif CASE == 3
  pthread_mutex_lock(&other);
  data = 2;
  pthread_mutex_unlock(&other);
#elif CASE == 4
  pthread_mutex_t *selected = __VERIFIER_nondet_int() ? &mutex : &other;
  pthread_mutex_lock(&mutex);
  pthread_mutex_unlock(selected);
  pthread_mutex_lock(&common);
  data = 2;
  pthread_mutex_unlock(&common);
  pthread_mutex_unlock(&mutex);
#elif CASE == 11
  pthread_mutex_lock(&common);
  data = 2;
  pthread_mutex_unlock(&common);
#else
  pthread_mutex_lock(&mutex);
#if CASE == 5
  pthread_mutex_lock(&mutex); /* ERRORCHECK does not increment recursion depth */
  pthread_mutex_unlock(&mutex);
#endif
  data = 2;
  pthread_mutex_unlock(&mutex);
#endif
  pthread_join(thread, 0);
  return 0;
}
#endif
