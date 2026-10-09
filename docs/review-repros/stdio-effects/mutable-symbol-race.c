#include <pthread.h>
#include <stdio.h>
char buffer[8] __asm__(".writable");
void fill(void) { sprintf(buffer, "%c", 'a'); }
void *format(void *arg) { fill(); return 0; }
void *write(void *arg) { buffer[0] = 'b'; return 0; }
int main(void) {
  pthread_t a, b;
  pthread_create(&a, 0, format, 0);
  pthread_create(&b, 0, write, 0);
  return 0;
}
