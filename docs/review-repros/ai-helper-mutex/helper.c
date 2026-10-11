#include <pthread.h>
#include <stdlib.h>
extern int __VERIFIER_nondet_int(void);
struct entry { int data; pthread_mutex_t mutex, other; } entries[4], A, B;
int choose(void) {
  int i = __VERIFIER_nondet_int();
  if (i < 0 || i >= 4) abort();
  return i;
}
void store_value(int *data) { *data = 2; }
void unsafe_update(struct entry *entry) {
#if CASE == 9
  if (__VERIFIER_nondet_int()) pthread_mutex_lock(&entry->mutex);
  entry->data = 2;
#elif CASE == 10
  pthread_mutex_unlock(&entry->mutex);
  entry->data = 2;
#elif CASE == 11
  entry = &entries[(choose() + 1) % 4];
  entry->data = 2;
#endif
}
void forward_value(int *data) { store_value(data); }
void release_lock(pthread_mutex_t *mutex) { pthread_mutex_unlock(mutex); }
void protected_update(struct entry *entry) {
#if CASE == 1
  pthread_mutex_lock(&entry->other);
#else
  pthread_mutex_lock(&entry->mutex);
#endif
#if CASE == 2
  pthread_mutex_unlock(&entry->mutex);
#endif
  entry->data = 2;
#if CASE == 1
  pthread_mutex_unlock(&entry->other);
#elif CASE != 2
  pthread_mutex_unlock(&entry->mutex);
#endif
}
void *worker(void *unused) {
#if CASE == 6
  struct entry *entry = &A;
#else
  struct entry *entry = &entries[choose()];
#endif
  pthread_mutex_lock(&entry->mutex);
  entry->data = 1;
  pthread_mutex_unlock(&entry->mutex);
  return 0;
}
int main(void) {
  for (int i = 0; i < 4; ++i) {
    pthread_mutex_init(&entries[i].mutex, 0);
    pthread_mutex_init(&entries[i].other, 0);
  }
  pthread_mutex_init(&A.mutex, 0);
  pthread_mutex_init(&B.mutex, 0);
  pthread_t thread;
  pthread_create(&thread, 0, worker, 0);
#if CASE <= 2
  protected_update(&entries[choose()]);
#elif CASE == 3 || CASE == 4 || CASE == 5 || CASE == 7
  struct entry *entry = &entries[choose()];
  pthread_mutex_lock(&entry->mutex);
#if CASE == 5
  release_lock(&entry->mutex);
#endif
#if CASE == 7
  forward_value(&entry->data);
#else
  store_value(&entry->data);
#endif
#if CASE != 5
  pthread_mutex_unlock(&entry->mutex);
#endif
#if CASE == 4
  store_value(&entry->data); /* Same callee statement, unprotected context. */
#endif
#elif CASE == 6
  struct entry *entry = __VERIFIER_nondet_int() ? &A : &B;
  pthread_mutex_lock(&entry->mutex);
  store_value(&entry->data);
  pthread_mutex_unlock(&entry->mutex);
#elif CASE == 8
  int i = choose();
  pthread_mutex_lock(&entries[i].mutex);
  store_value(&entries[(i + 1) % 4].data);
  pthread_mutex_unlock(&entries[i].mutex);
#elif CASE == 9
  unsafe_update(&entries[choose()]);
#elif CASE == 10 || CASE == 11
  struct entry *entry = &entries[choose()];
  pthread_mutex_lock(&entry->mutex);
  unsafe_update(entry);
#if CASE == 11
  pthread_mutex_unlock(&entry->mutex);
#endif
#endif
  pthread_join(thread, 0);
  return 0;
}
