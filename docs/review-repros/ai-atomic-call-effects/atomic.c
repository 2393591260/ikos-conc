#include <pthread.h>
#include <stddef.h>
#ifndef CASE
#define CASE 0
#endif
#ifdef RENAMED
#define flag renamed_flag
#define payload renamed_payload
#define reader renamed_reader
#define writer renamed_writer
#endif
#if CASE == 5
static __int128 flag;
#else
static unsigned long long flag;
#endif
static int payload;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
extern void abi_store(size_t, void *, void *, int) __asm__("__atomic_store");
#if CASE == 6
static void named_effect(void) __asm__("__atomic_custom_effect");
static void named_effect(void) { payload = 1; }
#elif CASE == 7
static void named_effect(void) __asm__("__sync_custom_effect");
static void named_effect(void) { payload = 1; }
#elif CASE == 8
extern void named_effect(void) __asm__("__atomic_custom_effect");
#elif CASE == 9
extern void __VERIFIER_atomic_write(void);
#endif
static void *reader(void *arg) {
  (void)arg;
#if CASE == 6
  named_effect();
#elif CASE == 7
  named_effect();
#elif CASE == 8
  named_effect();
#elif CASE == 9
  __VERIFIER_atomic_write();
#else
  pthread_mutex_lock(&mutex);
  flag = 0;
  pthread_mutex_unlock(&mutex);
  pthread_mutex_lock(&mutex);
#if CASE == 5
  __int128 x = flag;
#else
  unsigned long long x = flag;
#endif
#if CASE == 2
  if (x == 1) payload = 1;
#endif
  pthread_mutex_unlock(&mutex);
#if CASE != 2
  if (x == 1) payload = 1;
#endif
#endif
  return 0;
}
static void *writer(void *arg) {
  (void)arg;
#if CASE == 6
  named_effect();
#elif CASE == 7
  named_effect();
#elif CASE == 8
  named_effect();
#elif CASE == 9
  __VERIFIER_atomic_write();
#else
#if CASE == 5
  __int128 one = 1;
#else
  unsigned long long one = CASE == 1 ? 0 : 1;
#endif
  pthread_mutex_lock(&mutex);
#if CASE == 0 || CASE == 1 || CASE == 2
  abi_store(sizeof(flag), &flag, &one, __ATOMIC_SEQ_CST);
#elif CASE == 3
  flag = one; /* same source effect, covered typed Store control */
#else
  __atomic_store(&flag, &one, __ATOMIC_SEQ_CST);
#endif
#if CASE == 2
  payload = 2;
#endif
  pthread_mutex_unlock(&mutex);
#if CASE != 2
  payload = 2;
#endif
#endif
  return 0;
}
int main(void) {
  pthread_t a, b;
  pthread_create(&a, 0, reader, 0);
  pthread_create(&b, 0, writer, 0);
  pthread_join(a, 0);
  pthread_join(b, 0);
  return 0;
}
