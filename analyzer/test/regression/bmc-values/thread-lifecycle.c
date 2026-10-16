// Serial generations, helper return relations and transitive drain controls.
#include <pthread.h>
#include <stdlib.h>
#ifndef CASE
#define CASE 0
#endif
#ifdef WIDE
typedef long long datum;
#else
typedef int datum;
#endif
extern int __VERIFIER_nondet_int(void);
extern void opaque(void);
pthread_t outer, inner, another;
void *shared_status;
pthread_mutex_t mutex;
datum payload;
#if CASE == 38
long long phase;
#define EMPTY 4294967296LL
#define BUSY 4294967297LL
#elif CASE == 39
long long phase;
#define EMPTY 0LL
#define BUSY 4294967296LL
#else
int phase;
#define EMPTY 0
#define BUSY 1
#endif
#if CASE == 39
#define IS_EMPTY ((int)phase == (int)EMPTY)
#else
#define IS_EMPTY (phase == EMPTY)
#endif
#if CASE == 33
__asm__(".globl lifecycle_assembly_symbol");
#endif
#if CASE == 14
int pthread_join(pthread_t value, void **result) { (void)value; (void)result; return 0; }
#endif
static void touch(datum x) { payload = x; }
static void *leaf(void *unused) {
  (void)unused;
#if CASE == 8
  phase = EMPTY;
#endif
  while (__VERIFIER_nondet_int()) {
#if CASE != 36
    pthread_mutex_lock(&mutex);
#endif
    payload = 6;
#if CASE != 36
    pthread_mutex_unlock(&mutex);
#endif
  }
  return NULL;
}
static int register_child(void) {
  if (__VERIFIER_nondet_int()) {
    pthread_create(&inner, NULL, leaf, NULL);
#if CASE == 5
    pthread_create(&inner, NULL, leaf, NULL);
#endif
#if CASE == 31
    if (__VERIFIER_nondet_int()) return -1;
#endif
    return 0;
  }
  return -1;
}
static int probe(void) {
  int result = register_child();
  if (result != 0) return -1;
  return 0;
}
static void unregister_child(void) {
#if CASE == 17
  char status;
#else
  void *status;
#endif
#if CASE == 3
  if (__VERIFIER_nondet_int()) return;
#elif CASE == 12
  opaque();
#elif CASE == 15
  *(unsigned char *)&inner = 0;
#endif
#if CASE == 7
  pthread_join(inner, (void **)&payload);
#elif CASE == 20
  pthread_join(inner, &shared_status);
#elif CASE == 10
#if CASE == 25
  pthread_mutex_destroy(&mutex);
#endif
  pthread_join(outer, &status);
#elif CASE == 18
  pthread_join(inner, NULL);
#else
  pthread_join(inner, (void **)&status);
#endif
#if CASE == 16
  if (status) payload = 12;
#endif
}
static void disconnect(void) { unregister_child(); }
static void *parent(void *unused) {
  (void)unused;
  phase = EMPTY;
  while (1) {
    switch (__VERIFIER_nondet_int()) {
      case 0:
        if (IS_EMPTY) {
          if (probe() != 0) goto done;
          phase = BUSY;
#if CASE == 34
          touch(7);
#elif CASE == 27
          *(unsigned char *)&phase = EMPTY;
#endif
#if CASE == 1
          payload = 7;
#elif CASE == 4
          return NULL;
#elif CASE == 21
          phase = EMPTY;
#endif
        }
        break;
      case 1:
        if (phase == BUSY) {
#if CASE == 11
          pthread_mutex_lock(&mutex);
#endif
#if CASE == 6
          pthread_t old = inner;
#endif
          disconnect();
#if CASE == 6
          register_child();
          pthread_join(old, NULL);
#endif
#if CASE == 11
          pthread_mutex_unlock(&mutex);
#endif
          phase = EMPTY;
#if CASE == 34
          touch(8);
#else
          payload = 8;
#endif
        }
        break;
      case 2:
        if (IS_EMPTY) goto done;
        break;
    }
  }
done:
  payload = 9;
  return NULL;
}
static int start(void) {
#if CASE != 36
  pthread_mutex_init(&mutex, NULL);
#endif
  payload = 1;
  if (__VERIFIER_nondet_int()) {
    pthread_create(&outer, NULL, parent, NULL);
#if CASE == 9
    pthread_create(&another, NULL, parent, NULL);
#elif CASE == 29
    phase = EMPTY;
#endif
    return 0;
  }
  payload = 3;
#if CASE != 36
  pthread_mutex_destroy(&mutex);
#endif
  return -1;
}
static void finish(void) {
  void *status;
#if CASE == 22
  payload = 5;
#endif
#if CASE == 25
  pthread_mutex_destroy(&mutex);
#endif
  pthread_join(outer, &status);
#if CASE != 36
  pthread_mutex_destroy(&mutex);
#endif
  payload = 5;
}
int main(void) {
  if (start() != 0) return 0;
  finish();
  return 0;
}
