#include <pthread.h>

static int x = 1, y;
static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
struct Payload { ~Payload() { x = 2; } };

static void *worker(void *) {
  pthread_mutex_lock(&gate);
  if (x != 1) {
    pthread_mutex_unlock(&gate);
    y = 1;
  } else {
    pthread_mutex_unlock(&gate);
  }
  return 0;
}

int main() {
  pthread_t a, b;
  pthread_create(&a, 0, worker, 0);
  pthread_create(&b, 0, worker, 0);
  pthread_mutex_lock(&gate);
  try { throw Payload{}; } catch (...) {}
  pthread_mutex_unlock(&gate);
  pthread_join(a, 0);
  pthread_join(b, 0);
}
