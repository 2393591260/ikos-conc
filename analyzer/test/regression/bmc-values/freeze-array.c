// Inspect zero-length lowering without executing zero-length C VLAs natively.
#include <pthread.h>
#include <stdlib.h>
#ifndef CASE
#define CASE 0
#endif
#ifdef WIDE
typedef long long datum;
#else
typedef int datum;
#endif
extern int __VERIFIER_nondet_int(void);
static int shared;
static void require(int condition) { if (!condition) abort(); }
static void *writer(void *arg) { (void)arg; shared = 1; return 0; }
int main(void) {
  long long count = __VERIFIER_nondet_int();
#if CASE == 3
  require(count == 0);
#else
  require(count == 1);
#endif
#ifdef SCALAR
  datum cell;
#define VALUE cell
#else
  datum array[count];
#define VALUE array[0]
#if CASE == 3
  datum *volatile observed = array;
  (void)observed; // Keep the zero-count allocation without accessing a cell.
#endif
#endif
#if CASE == 2
  VALUE = 0;
#endif
#if CASE == 1
  if (VALUE == 0 && VALUE != 0) {
#elif CASE == 2
  if (VALUE != 0) {
#elif CASE == 3
  {
#else
  if (VALUE == 0) {
#endif
    pthread_t first, second;
    pthread_create(&first, 0, writer, 0);
    pthread_create(&second, 0, writer, 0);
  }
  return 0;
}
