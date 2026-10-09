#include <pthread.h>
#include <stdio.h>
int data;
void *output(void *arg) { printf("%d\n", 7); puts("literal"); return 0; }
void *write(void *arg) { data = 1; return 0; }
int main(void) {
  pthread_t a, b;
  pthread_create(&a, 0, output, 0);
  pthread_create(&b, 0, write, 0);
  return 0;
}
