/* MAINTAINED_CONTROL_SPECIFICATION: source/native contract refusal, both ABIs. */
typedef __UINTPTR_TYPE__ pthread_t;
typedef int pthread_once_t;
extern int pthread_create(pthread_t *, const void *, void *(*)(void *), void *);
extern int pthread_join(pthread_t, void **);
static int g;
static pthread_once_t once;
static void init(void) { g++; }
extern int pthread_once(pthread_once_t *, void (*)(void));
static void *worker(void *p) { (void)p; pthread_once((pthread_once_t *)p, init); return 0; }
int main(void) { pthread_t t; pthread_once_t local = 0; pthread_create(&t,0,worker,&local); g++; pthread_join(t,0); return 0; }
