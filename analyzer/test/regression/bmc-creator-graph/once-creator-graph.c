/* Bounded once/Create controls.
 * CASE0 callback-after-create race;1 callback-before-create ordered;
 * CASE2 both payloads same mutex;3 real join orders (G0 unsupported);
 * CASE4 child passive once return orders callback payload before child.
 */
#include <pthread.h>
#ifndef CASE
#define CASE 0
#endif
static int payload;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static void initialize(void);
static void *child(void *unused) {
  (void)unused;
#if CASE == 4
  pthread_once(&once, initialize);
#endif
#if CASE == 2
  pthread_mutex_lock(&mutex);
#endif
  payload = 17;
#if CASE == 2
  pthread_mutex_unlock(&mutex);
#endif
  return 0;
}
static void initialize(void) {
  pthread_t local_id;
#if CASE == 1
  payload = 42;
#endif
  pthread_create(&local_id, 0, child, 0);
#if CASE == 3
  pthread_join(local_id, 0);
#endif
#if CASE == 2
  pthread_mutex_lock(&mutex);
#endif
#if CASE != 1
  payload = 42;
#endif
#if CASE == 2
  pthread_mutex_unlock(&mutex);
#endif
}
static void *worker(void *unused) {
  (void)unused;
  pthread_once(&once, initialize);
  return 0;
}
int main(void) {
  pthread_t id;
  pthread_create(&id, 0, worker, 0);
  pthread_once(&once, initialize);
  return 0;
}
