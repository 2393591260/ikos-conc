#include <pthread.h>
#ifndef CASE
#define CASE 0
#endif
extern int __VERIFIER_nondet_int(void);
static int payload;
static pthread_t child, other;
static void *writer(void *p) { payload = 1; return 0; }
static void *benign(void *p) { return 0; }
static int factory(void) {
#if CASE != 6 && CASE != 12 && CASE != 15
  if (__VERIFIER_nondet_int()) return -1;
#endif
#if CASE == 6
  int status = -1;
#endif
  pthread_create(&child, 0, writer, 0);
#if CASE == 5 || CASE == 12
  return -1;
#elif CASE == 6
  return status;
#elif CASE == 15
  return 256;
#else
  return 0;
#endif
}
static void *parent(void *p) {
  pthread_create(&child, 0, writer, 0);
#if CASE == 8
  pthread_join(child, 0);
#endif
  return 0;
}
static void launch(pthread_t *out, void *(*start)(void *)) {
  pthread_create(out, 0, start, 0);
}
static void *creator(void *p) {
  pthread_t id;
#if CASE == 20
  launch(&id, writer);
#else
  pthread_create(&id, 0, writer, 0);
#endif
  return 0;
}
int main(void) {
#if CASE == 0 || CASE == 5 || CASE == 6
  if (factory() == 0) pthread_join(child, 0);
#elif CASE == 1
  if (!__VERIFIER_nondet_int()) {
    pthread_create(&child, 0, writer, 0);
    pthread_join(child, 0);
  }
#elif CASE == 2
  if (factory() == 0) { /* missing join */ }
#elif CASE == 3
  pthread_create(&other, 0, benign, 0);
  if (factory() == 0) pthread_join(other, 0);
#elif CASE == 4
  if (factory() == 0 && __VERIFIER_nondet_int()) pthread_join(child, 0);
#elif CASE == 7 || CASE == 8
  pthread_t outer;
  pthread_create(&outer, 0, parent, 0);
  pthread_join(outer, 0);
#elif CASE == 9
  if (factory() == 0) pthread_join(child, 0);
  pthread_create(&other, 0, writer, 0);
#elif CASE == 10
  for (int i = 0; i < 2; ++i) pthread_create(&child, 0, writer, 0);
  pthread_join(child, 0);
#elif CASE == 11
  if (factory() == 0) { payload = 3; pthread_join(child, 0); }
#elif CASE == 12
  if (factory() != 0) pthread_join(child, 0);
#elif CASE == 13
  if (factory() == 0) pthread_join(child, 0);
  pthread_create(&other, 0, creator, 0);
#elif CASE == 14
  factory();
  if (factory() == 0) pthread_join(child, 0);
#elif CASE == 15
  unsigned char status = factory();
  if (status != 0) pthread_join(child, 0);
#elif CASE == 16
  int status = factory();
  status = 1;
  if (status == 0) pthread_join(child, 0);
#elif CASE == 17
  long status = factory();
  if (status == 0) pthread_join(child, 0);
#elif CASE == 18
  if (factory() == 0) pthread_join(child, 0);
  pthread_create(&other, 0, benign, 0);
  pthread_join(other, 0);
#elif CASE == 20
  pthread_create(&child, 0, writer, 0);
  pthread_join(child, 0);
  pthread_create(&other, 0, creator, 0);
#elif CASE == 21
  for (int i = 0; i < 2; ++i) {
    pthread_create(&child, 0, writer, 0);
    pthread_join(child, 0);
  }
#elif CASE == 22
  pthread_create(&child, 0, writer, 0);
  pthread_create(&other, 0, writer, 0);
  pthread_join(child, 0);
#elif CASE == 19
  void *(*start)(void *) = __VERIFIER_nondet_int() ? writer : benign;
  pthread_create(&other, 0, start, 0);
#endif
  payload = 2;
  return 0;
}
