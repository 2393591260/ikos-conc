#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#ifndef CASE
#define CASE 0
#endif
#if CASE == 1
#define reader z_reader
#define writer a_writer
#endif
struct state { _Atomic int flag; int value; };
#if CASE == 2
static struct state shared;
#define FLAG shared.flag
#define VALUE shared.value
#elif CASE == 3
static struct state *shared;
#define FLAG shared->flag
#define VALUE shared->value
#else
#if CASE == 5
static _Thread_local _Atomic int flag;
#else
static _Atomic int flag;
#endif
static int value;
#define FLAG flag
#define VALUE value
#endif
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static void *reader(void *unused) {
#if CASE == 4
  _Atomic int local_flag;
#define READ_FLAG local_flag
#else
#define READ_FLAG FLAG
#endif
  atomic_store(&READ_FLAG, 0);
  if (atomic_load(&READ_FLAG) == 1) {
#if CASE == 7
    pthread_mutex_lock(&mutex);
#endif
    VALUE = 1;
#if CASE == 7
    pthread_mutex_unlock(&mutex);
#endif
  }
  return 0;
}
static void *writer(void *unused) {
#if CASE != 4 && CASE != 6
  atomic_store(&FLAG, 1);
#endif
#if CASE == 7
  pthread_mutex_lock(&mutex);
#endif
  VALUE = 2;
#if CASE == 7
  pthread_mutex_unlock(&mutex);
#endif
  return 0;
}
int main(void) {
#if CASE == 3
  shared = malloc(sizeof(*shared));
  if (!shared) return 0;
  atomic_init(&shared->flag, 0);
#endif
  pthread_t a, b;
  pthread_create(&a, 0, reader, 0);
  pthread_create(&b, 0, writer, 0);
  pthread_join(a, 0);
  pthread_join(b, 0);
  return 0;
}
