#include <pthread.h>
#include <stdint.h>

#ifndef CASE
#define CASE 0
#endif
#ifndef VARIANT
#define VARIANT 0
#endif
#if CASE == 1
#define COUNT 10
#else
#define COUNT 20
#endif
#if VARIANT == 1
#define writer renamed_producer
#define reader renamed_consumer
#define data shared_cell
#define index creation_counter
typedef uint64_t cell_type;
#else
typedef int cell_type;
#endif

extern unsigned __VERIFIER_nondet_uint(void);
extern void __VERIFIER_assume(int);
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t flag_mutex = PTHREAD_MUTEX_INITIALIZER;
static cell_type data, observed;
static int index, flag;

static void *writer(void *unused) {
  pthread_mutex_lock(&mutex);
  data = 1;
  pthread_mutex_unlock(&mutex);
  return 0;
}

static void *reader(void *unused) {
#if CASE == 2
  if (index == COUNT) return 0;
#endif
  observed = data;
  return 0;
}

static void *publish_flag(void *unused) {
  pthread_mutex_lock(&flag_mutex);
  flag = 1;
  pthread_mutex_unlock(&flag_mutex);
  return 0;
}

int main(void) {
  pthread_t writers[COUNT], consumer, prerequisite;
#if CASE == 1
  unsigned unrelated = __VERIFIER_nondet_uint();
  __VERIFIER_assume(unrelated < 32);
#endif
#if CASE == 3
  pthread_create(&prerequisite, 0, publish_flag, 0);
  pthread_mutex_lock(&flag_mutex);
  int ready = flag;
  pthread_mutex_unlock(&flag_mutex);
  if (!ready) return 0;
#endif
  for (index = 0; index < COUNT; ++index)
    pthread_create(&writers[index], 0, writer, 0);
  pthread_create(&consumer, 0, reader, 0);
  pthread_join(consumer, 0);
#if CASE == 1
  volatile int scratch = 0;
  for (int j = 0; j < 32; ++j) scratch = j;
#endif
  return 0;
}
