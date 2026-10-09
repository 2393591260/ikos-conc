#include <pthread.h>
extern int __VERIFIER_nondet_int(void);

int data, other;
void* worker(void* arg) {
  int choice = __VERIFIER_nondet_int();
  if (choice == 7 || choice == 9)
    data = choice;
  if (choice == 7)
    other = 8;
  return 0;
}
int main(void) {
  pthread_t a, b;
  pthread_create(&a, 0, worker, 0);
  pthread_create(&b, 0, worker, 0);
  return 0;
}
