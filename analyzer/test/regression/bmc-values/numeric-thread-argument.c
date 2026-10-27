#include <pthread.h>
#include <stdint.h>

int data[2];
void *escaped;
extern void sink(void *);
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

void *entry(void *arg) {
#if CASE == 2
  *(int *)arg = 1;
#elif CASE == 3
  escaped = arg;
#elif CASE == 4
  sink(arg);
#elif CASE == 5
  data[(intptr_t)((char *)arg + 1) & 1] = 1;
#elif CASE == 6
  return arg;
#elif CASE == 7
  if (arg != 0) data[0] = 1;
#elif CASE == 14
  intptr_t bits = (intptr_t)arg;
  *(int *)bits = 1;
#elif CASE == 21
  int index=(int)arg;
  data[index/4]=1;
#elif CASE == 18
#if UINTPTR_MAX == UINT64_MAX
  int32_t bits = (int32_t)arg;
#else
  int64_t bits = (int64_t)arg;
#endif
  data[bits & 1] = 1;
#else
#ifdef WIDE
  long long index = (intptr_t)arg;
#else
  intptr_t index = (intptr_t)arg;
#endif
#if CASE == 1
  pthread_mutex_lock(&lock);
#endif
#if CASE == 16
  if ((uintptr_t)index == (uintptr_t)-1) data[0] = 1;
#else
  data[index / 4] = 1;
#endif
#if CASE == 1
  pthread_mutex_unlock(&lock);
#endif
#endif
  return 0;
}

#if CASE == 12
static void start(pthread_t *thread, void *arg) {
  pthread_create(thread, 0, entry, arg);
}
#elif CASE == 17
static void *pack(intptr_t value) { return (void *)value; }
#elif CASE == 20
static void *other(void *arg) { *(int *)arg = 1; return 0; }
#endif

int main(void) {
  pthread_t threads[2];
#ifdef WIDE
  for (long long counter = 2; counter < 4; ++counter) {
    intptr_t index = (intptr_t)counter;
#else
  for (intptr_t index = 2; index < 4; ++index) {
#endif
#if CASE == 15
    void *arg = (void *)(long long)(index + 0x100000000LL);
#elif CASE == 16
    void *arg = (void *)(intptr_t)(index / 4 - 1);
#elif CASE == 17
    void *arg = pack(index);
#else
    void *arg = (void *)index;
#endif
#if CASE == 8
    escaped = arg;
#endif
#if CASE == 12
    start(&threads[index - 2], arg);
#else
    pthread_create(&threads[index - 2], 0, entry, arg);
#endif
#if CASE == 20
    pthread_t extra;
    pthread_create(&extra, 0, other, arg);
#endif
  }
  pthread_join(threads[0], 0);
  pthread_join(threads[1], 0);
  return 0;
}
