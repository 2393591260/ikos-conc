/* Distinct physical parents/children use the same static entry and Create.
 * CASE0 child race; CASE1 common mutex; CASE2 non-NULL child arg unsupported.
 */
#include <pthread.h>
#ifndef CASE
#define CASE 0
#endif
static int payload;
static pthread_t parents[2];
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static void *child(void *unused) {
  (void)unused;
#if CASE == 1
  pthread_mutex_lock(&mutex);
#endif
  payload = 1;
#if CASE == 1
  pthread_mutex_unlock(&mutex);
#endif
  return 0;
}
static void *parent(void *unused) {
  (void)unused;
  pthread_t local_id;
#if CASE == 2
  pthread_create(&local_id, 0, child, &payload);
#else
  pthread_create(&local_id, 0, child, 0);
#endif
  return 0;
}
int main(void) {
  for (int i = 0; i < 2; ++i) pthread_create(&parents[i], 0, parent, 0);
  return 0;
}
