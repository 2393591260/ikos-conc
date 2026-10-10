#include <pthread.h>
#ifndef CASE
#define CASE 0
#endif
#if CASE == 3
extern void __assert_fail(int);
#else
extern void __assert_fail(const char *, const char *, unsigned, const char *);
extern void __assert_perror_fail(int, const char *, unsigned, const char *);
extern void __assert(const char *, const char *, int);
#endif
static int data;
static char buffer[4]="abc";
static void *worker(void *arg) {
  (void)arg;
#if CASE == 4
  buffer[0]='z';
#else
  data=1;
#endif
  return 0;
}
int main(void) {
  pthread_t thread;
  pthread_create(&thread,0,worker,0);
#if CASE == 0
  __assert_fail("failed", "file.c", 1U, "main");
#elif CASE == 1
  __assert_perror_fail(1,"file.c",1U,"main");
#elif CASE == 2
  __assert("failed","file.c",1);
#elif CASE == 3
  __assert_fail(1);
#else
  __assert_fail(buffer,"file.c",1U,"main");
#endif
  data=2; /* unreachable for a valid libc termination hook */
  pthread_join(thread,0); return 0;
}
