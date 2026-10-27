/* Outside unexecuted control: actual pthread_once has a body, reject API seal. */
#include <pthread.h>
static int datum;
static pthread_once_t control = PTHREAD_ONCE_INIT;
static void callback(void) { ++datum; }
int pthread_once(pthread_once_t *object, void (*routine)(void)) {
  (void)object; routine(); routine(); return 0;
}
int main(void) { pthread_once(&control, callback); return 0; }
