#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
extern uintptr_t __VERIFIER_nondet_ulong(void);

#ifndef CASE
#define CASE 0
#endif
#ifdef WIDE
typedef int64_t value_type;
#else
typedef int value_type;
#endif

struct member { value_type value; };
struct object {
  pthread_mutex_t lock, other;
  char padding[3];
  struct member member;
};
static struct member *published;
static pthread_t first, second;

static void *worker(void *arg) {
  struct member *p = published;
  if (!p) return 0;
  struct object *base = (struct object *)((char *)p - offsetof(struct object, member));
#if CASE == 1
  pthread_mutex_t *lock = arg ? &base->other : &base->lock;
#else
  pthread_mutex_t *lock = &base->lock;
#endif
  pthread_mutex_lock(lock);
  base->member.value = 1;
  pthread_mutex_unlock(lock);
  return 0;
}

#if CASE == 5
static void set_slot(struct member **slot, struct member *value) { *slot = value; }
#endif
#if CASE == 7
extern void opaque_effect(void);
#endif

static void start(struct object *base) {
#if CASE == 3
  pthread_create(&first, 0, worker, 0);
#endif
#if CASE == 5
  set_slot(&published, &base->member);
#elif CASE == 8
  struct member *temporary = &base->member;
  memcpy(&published, &temporary, sizeof(published));
#else
  published = &base->member;
#endif
#if CASE != 3
  pthread_create(&first, 0, worker, 0);
#endif
  pthread_create(&second, 0, worker, base);
#if CASE == 2
  published = &base->member;
#elif CASE == 6
  unsigned char *bytes = (unsigned char *)&published;
  unsigned char saved = bytes[0];
  bytes[0] = saved;
#elif CASE == 7
  opaque_effect();
#elif CASE == 10
  *(struct member **)(__VERIFIER_nondet_ulong()) = &base->member;
#endif
}

int main(void) {
  struct object object;
  pthread_mutex_init(&object.lock, 0);
  pthread_mutex_init(&object.other, 0);
  object.member.value = 0;
  start(&object);
#if CASE == 9
  worker(0);
#endif
  pthread_join(first, 0);
  pthread_join(second, 0);
#if CASE == 4
  start(&object);
  pthread_join(first, 0);
  pthread_join(second, 0);
#endif
  return 0;
}
