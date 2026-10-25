#ifndef CASE
#define CASE 0
#endif

#if CASE >= 10 && CASE <= 13
#define pthread_detach declared_pthread_detach
#endif
#include <pthread.h>
#include <stdint.h>
#undef pthread_detach

#if CASE == 10
#if __SIZEOF_POINTER__ == 8
extern int pthread_detach(unsigned int);
#else
extern int pthread_detach(unsigned long long);
#endif
#elif CASE == 11
extern int pthread_detach(pthread_t, ...);
#elif CASE == 12
#if __SIZEOF_POINTER__ == 8
extern int __attribute__((ms_abi)) pthread_detach(pthread_t);
#else
extern int __attribute__((stdcall)) pthread_detach(pthread_t);
#endif
#endif

#if CASE == 18
extern int pthread_create(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*)
    __attribute__((nonnull(2)));
#elif CASE == 19
extern int pthread_detach(pthread_t) __attribute__((noreturn));
#endif

#ifdef WIDE
long long shared;
#else
int shared;
#endif
pthread_t escaped;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
extern void opaque(pthread_t*);

#if CASE == 21
__asm__(".text\n.globl pthread_detach\npthread_detach:\n"
        "movl $1, shared\nxorl %eax, %eax\nret\n");
#endif

#if CASE == 13
int pthread_detach(pthread_t thread) {
  (void)thread;
  shared++;
  return 0;
}
#endif

#if CASE == 16
static void* worker(void*) __attribute__((nonnull(1)));
#elif CASE == 17
static void* worker(void*) __attribute__((noreturn));
#endif

__attribute__((noinline)) static void intervening(void) { shared++; }

static void* worker(void* ignored) {
  (void)ignored;
#if CASE == 1 || CASE == 13 || CASE == 21 || CASE == 23
  pthread_mutex_lock(&mutex);
#endif
  shared++;
#if CASE == 1 || CASE == 13 || CASE == 21 || CASE == 23
  pthread_mutex_unlock(&mutex);
#endif
  return 0;
}

int main(void) {
  pthread_t thread;
#if CASE == 2
  pthread_detach((pthread_t)123);
#endif
#ifdef WIDE
  for (unsigned long long i = 0; i < (CASE == 22 || CASE == 23 ? 1 : 2); ++i) {
#else
  for (unsigned i = 0; i < (CASE == 22 || CASE == 23 ? 1 : 2); ++i) {
#endif
    pthread_create(&thread, 0, worker, 0);
#if CASE == 20
    intervening();
#endif
#if CASE == 3
    escaped = thread;
#elif CASE == 4
    opaque(&thread);
#elif CASE == 5
    thread = (pthread_t)7;
#elif CASE == 14
    (void)*(volatile pthread_t*)&thread;
#endif
#if CASE == 6
    int result = pthread_detach(thread);
    if (result) shared = result;
#elif CASE == 13
    pthread_detach(0);
#else
    pthread_detach(thread);
#endif
#if CASE == 7
    pthread_detach(thread);
#elif CASE == 8
    pthread_join(thread, 0);
#elif CASE == 9
    if (thread == escaped) shared = 3;
#endif
  }
#if CASE == 15
  pthread_t other;
  pthread_create(&other, 0, worker, 0);
  pthread_join(other, 0);
#endif
#if CASE == 22
  shared++;
#elif CASE == 23
  pthread_mutex_lock(&mutex);
  shared++;
  pthread_mutex_unlock(&mutex);
#endif
  return 0;
}
