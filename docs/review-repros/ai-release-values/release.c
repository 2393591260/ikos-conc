#include <pthread.h>
#include <stdint.h>
#include <stddef.h>

#ifndef CASE
#define CASE 0
#endif
#ifdef WIDE
typedef int64_t value_type;
#else
typedef int value_type;
#endif
extern int __VERIFIER_nondet_int(void);
extern void *__VERIFIER_nondet_pointer(void);
extern void __ikos_forget_mem(void *, size_t);
extern void __ikos_abstract_mem(void *, size_t);
static value_type x = 1, y;
static pthread_mutex_t outer = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t inner = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition = PTHREAD_COND_INITIALIZER;

static void *reader(void *arg) {
#if CASE == 5
  int locked = __VERIFIER_nondet_int();
  if (locked) pthread_mutex_lock(&outer);
#else
  pthread_mutex_lock(&outer);
#endif
#if CASE == 7
  pthread_cond_wait(&condition, &outer);
#endif
  if (x != 1) {
#if CASE == 5
    if (locked) pthread_mutex_unlock(&outer);
#else
    pthread_mutex_unlock(&outer);
#endif
    y = 1;
  } else {
#if CASE == 5
    if (locked) pthread_mutex_unlock(&outer);
#else
    pthread_mutex_unlock(&outer);
#endif
  }
  return 0;
}

#if CASE == 9
static void store(value_type *p, value_type v) { *p = v; }
#endif

int main(void) {
  pthread_t first, second;
  pthread_create(&first, 0, reader, 0);
  pthread_create(&second, 0, reader, 0);
#if CASE == 4
  pthread_mutex_lock(&inner);
#else
  pthread_mutex_lock(&outer);
#endif
#if CASE == 3 || CASE == 10
  pthread_mutex_lock(&inner);
#endif
#if CASE == 9
  store(&x, 0);
  store(&x, 1);
#else
  x = 0;
#if CASE == 3
  pthread_mutex_unlock(&inner);
  pthread_mutex_lock(&inner);
#endif
  x = CASE == 1 ? 2 : 1;
#endif
#if CASE == 11
  __ikos_forget_mem(__VERIFIER_nondet_pointer(), sizeof(x));
#elif CASE == 12
  __ikos_abstract_mem(__VERIFIER_nondet_pointer(), sizeof(x));
#elif CASE == 13
  pthread_create((pthread_t *)__VERIFIER_nondet_pointer(), 0, reader, 0);
#elif CASE == 14
  __ikos_abstract_mem(__VERIFIER_nondet_pointer(), 0);
#elif CASE == 15
  value_type private_cell;
  __ikos_abstract_mem(&private_cell, sizeof(private_cell));
#endif
#if CASE == 10
  pthread_mutex_unlock(__VERIFIER_nondet_int() ? &outer : &inner);
  x = 2;
#elif CASE == 3
  pthread_mutex_unlock(&inner);
#endif
#if CASE == 4
  pthread_mutex_unlock(&inner);
#else
  pthread_mutex_unlock(&outer);
#endif
#if CASE == 2
  x = 0;
#elif CASE == 6
  pthread_mutex_lock(&outer);
  x = 0;
  x = 1;
  pthread_mutex_unlock(&outer);
#elif CASE == 8
  ((unsigned char *)&x)[0] = 0;
#endif
#if CASE == 16
  void *return_value;
  pthread_join(first, &return_value);
#else
  pthread_join(first, 0);
#endif
  pthread_join(second, 0);
  return 0;
}
