#include <pthread.h>
#include <stdio.h>
char data[8] = "a";
void *format(void *arg) { printf("%s", data); return 0; }
void *write(void *arg) { data[0] = 'b'; return 0; }
int main(void) {
  pthread_t a, b;
  pthread_create(&a, 0, format, 0);
  pthread_create(&b, 0, write, 0);
  return 0;
}
