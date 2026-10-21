#include <pthread.h>
#include <stdint.h>

extern int __VERIFIER_nondet_int(void);
extern long long __VERIFIER_nondet_longlong(void);
extern void sink(void *);
int data;
void *escaped;
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static void *worker(void *unused) {
#ifdef WIDE
  uintptr_t bits = (uintptr_t)__VERIFIER_nondet_longlong();
#else
  uintptr_t bits = (uintptr_t)__VERIFIER_nondet_int();
#endif
#if CASE == 2
  bits = (uintptr_t)(signed char)bits;
#elif CASE == 3
  bits = (uintptr_t)0x100000000ULL;
#elif CASE == 4
  bits = (uintptr_t)0x100000001ULL;
#elif CASE == 9
  bits = (uintptr_t)-1;
#endif
  void *pointer = (void *)bits;
#if CASE == 10
  *(int *)pointer = 1;
#elif CASE == 11
  escaped = pointer;
#elif CASE == 12
  return pointer;
#elif CASE == 13
  sink(pointer);
#elif CASE == 14
  pointer = (char *)pointer + 1;
#elif CASE == 15
  if (pointer == (void *)&data) data = 1;
#elif CASE == 16
  if (pointer < (void *)&data) data = 1;
#elif CASE == 17
  bits = (uintptr_t)pointer;
#elif CASE == 18
  pointer = bits & 1 ? pointer : (void *)&data;
#elif CASE == 19
  __asm__ volatile("" : "+r"(pointer));
#endif
#if CASE == 5
  if (pointer == 0 && bits != 0) data = 1;
#elif CASE == 6
  if (pointer != 0 && bits == 0) data = 1;
#elif CASE == 7
  for (int i = 0; i < 2; ++i) {
    uintptr_t next = (uintptr_t)__VERIFIER_nondet_int();
    void *token = (void *)next;
    if (i == 0) {
      if (token != 0) return 0;
    } else {
      if (token == 0) return 0;
      data = 1;
    }
  }
#elif CASE == 8
  if (0 == (char *)pointer) data = 1;
#else
  if (pointer != 0) {
#if CASE == 1
    pthread_mutex_lock(&lock);
#endif
    data = 1;
#if CASE == 1
    pthread_mutex_unlock(&lock);
#endif
  }
#endif
  return 0;
}

int main(void) {
  pthread_t a, b;
  pthread_create(&a, 0, worker, 0);
  pthread_create(&b, 0, worker, 0);
  pthread_join(a, 0);
  pthread_join(b, 0);
  return 0;
}
