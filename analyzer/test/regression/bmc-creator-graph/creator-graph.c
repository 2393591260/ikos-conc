/* Bounded creator graph controls.
 * CASE0 nested race;1 same-mutex safe;2 parent-join containment safe (G0 unsupported);
 * CASE3 two nested parent levels race;4 unreachable nested child, no FALSE.
 * Compile with actual pthread headers for the selected ABI.
 */
#include <pthread.h>
#ifndef CASE
#define CASE 0
#endif
static int payload;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t first_id, parent_id, child_id, middle_id;
static void *child(void *unused) {
  (void)unused;
#if CASE == 1
  pthread_mutex_lock(&mutex);
#endif
  ++payload;
#if CASE == 1
  pthread_mutex_unlock(&mutex);
#endif
  return 0;
}
static void *middle(void *unused) {
  (void)unused;
  pthread_create(&child_id, 0, child, 0);
  return 0;
}
static void *parent(void *unused) {
  (void)unused;
#if CASE == 2
  pthread_mutex_lock(&mutex);
#endif
#if CASE == 3
  pthread_create(&middle_id, 0, middle, 0);
#elif CASE == 4
  if (0) pthread_create(&child_id, 0, child, 0);
#else
  pthread_create(&child_id, 0, child, 0);
#endif
#if CASE == 2
  pthread_join(child_id, 0);
  pthread_mutex_unlock(&mutex);
#endif
  return 0;
}
static void *first(void *unused) {
  (void)unused;
  pthread_mutex_lock(&mutex);
  ++payload;
  pthread_mutex_unlock(&mutex);
  return 0;
}
int main(void) {
  pthread_create(&first_id, 0, first, 0);
  pthread_create(&parent_id, 0, parent, 0);
  return 0;
}
