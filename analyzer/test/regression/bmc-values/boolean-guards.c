// Normal-return guards and their failure boundaries, independent of helper names.
#include <stdlib.h>
extern int __VERIFIER_nondet_int(void);
extern void __ikos_assert(int);
#ifndef CASE
#define CASE 0
#endif
#ifdef WIDE
typedef long long index_type;
#else
typedef int index_type;
#endif
int copy_condition(int condition) { return condition; }
void check_condition(int condition) {
#if CASE == 7
  (void)condition;
#else
  if (!condition) {
#if CASE == 2
    if (__VERIFIER_nondet_int()) return;
#endif
    abort();
  }
#endif
}
int main(void) {
  index_type index = __VERIFIER_nondet_int();
#if CASE == 8 || CASE == 9
  for (int repeat = 0; repeat < 2; ++repeat) {
    index = __VERIFIER_nondet_int();
#endif
#if CASE == 1
    check_condition(index >= 0 || index < 5);
#elif CASE == 4
    check_condition(copy_condition(index >= 0 && index < 5));
#elif CASE == 5
    unsigned char byte = (unsigned char)(index >= 0 && index < 5);
    check_condition((int)byte);
#elif CASE == 6
    check_condition((unsigned char)index);
#else
    check_condition(index >= 0 && index < 5);
#endif
#if CASE == 3 || CASE == 9
    index = __VERIFIER_nondet_int();
#endif
    __ikos_assert(index >= 0 && index < 5);
#if CASE == 8 || CASE == 9
  }
#endif
  return 0;
}
