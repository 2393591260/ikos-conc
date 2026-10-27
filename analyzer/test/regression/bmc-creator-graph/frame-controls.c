/* CASE0 returned helper-local handle does not terminate its child.
 * CASE1 repeated helper with alloca; CASE2 repeated helper without alloca;
 * CASE3 dead escaped output; CASE4 misaligned output; CASE5 one-past output.
 * CASE1..5 are outside the first graph profile, not safety certificates.
 */
#include <pthread.h>
#ifndef CASE
#define CASE 0
#endif
static int payload, private_marker;
static pthread_t main_id;
static pthread_t *escaped;
static void *child(void *unused) { (void)unused; payload = 17; return 0; }
__attribute__((noinline)) static void issue(void) {
  pthread_t local_id;
#if CASE == 3
  escaped = &local_id;
#else
  pthread_create(&local_id, 0, child, 0);
#endif
}
__attribute__((noinline)) static void marker(void) { ++private_marker; }
static void *parent(void *unused) {
  (void)unused;
#if CASE == 2
  marker(); marker();
#endif
#if CASE == 4
  char storage[2 * sizeof(pthread_t)] __attribute__((aligned(16)));
  pthread_create((pthread_t *)(storage + 1), 0, child, 0);
#elif CASE == 5
  pthread_t local_id;
  pthread_create(&local_id + 1, 0, child, 0);
#else
  issue();
#if CASE == 1
  issue();
#elif CASE == 3
  pthread_create(escaped, 0, child, 0);
#endif
#endif
  payload = 42;
  return 0;
}
int main(void) { pthread_create(&main_id, 0, parent, 0); return 0; }
