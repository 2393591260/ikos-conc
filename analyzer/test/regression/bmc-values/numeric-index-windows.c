// Integer-token windows, truncation boundaries and broken-premise controls.
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#ifndef CASE
#define CASE 0
#endif
extern int __VERIFIER_nondet_int(void);
static void assume_abort_if_not(int condition) { if (!condition) abort(); }
static void *safe_malloc(size_t size) { void *p=malloc(size); if (!p) abort(); return p; }
#if CASE == 1
typedef long long datum;
#else
typedef int datum;
#endif
static datum *data;
#if CASE == 24
static datum *other;
#endif
#if CASE == 25
static datum **slot_alias = &data;
#endif
#if CASE == 27
typedef long long unaligned_wide __attribute__((aligned(1), may_alias));
#endif
#if CASE == 29
extern void opaque_write(void *);
#endif
#if CASE == 26
static void *shifted_malloc(size_t size) {
  return (datum *)safe_malloc(size + sizeof(datum)) + 1;
}
#endif
static void mutate(datum **slot) { if (__VERIFIER_nondet_int()) *slot = *slot + 1; }
static void *worker(void *argument) {
#if CASE == 2
  int index=(int)(intptr_t)argument;
#elif CASE == 10
  unsigned char index=(unsigned char)(uintptr_t)argument;
#elif CASE == 11
  intptr_t index=0;
#elif CASE == 12
  intptr_t index=((intptr_t)argument)&1;
#elif CASE == 15 || CASE == 30 || CASE == 31
  int index=(int)argument; // Direct LLVM ptrtoint32, including LP64.
#elif CASE == 20 || CASE == 21
  signed char index=(signed char)argument;
#elif CASE == 22 || CASE == 23
  unsigned char index=(unsigned char)argument;
#else
  intptr_t index=(intptr_t)argument;
#endif
#if CASE == 27
  *(unaligned_wide *)&data[index]=1; // Eight bytes over a four-byte stride.
#else
  data[index]=1;
#endif
#if CASE == 24
  other[index]=2; // A different slot must not borrow data's base certificate.
#endif
#if CASE == 25
  if (__VERIFIER_nondet_int()) *slot_alias = data + 1;
#endif
#if CASE == 28
  if ((uintptr_t)argument) *(volatile unsigned char *)argument=7;
#endif
#if CASE == 29
  opaque_write(data); // Heap address escapes; slot remains syntactically closed.
#endif
#if CASE == 13
  data[index+1]=2;
#endif
  return 0;
}
int main(void) {
#if CASE == 30 || CASE == 31
  // LP64 symbolic range controls; never execute these C programs natively.
  long long count=2147483648LL+(CASE==31);
#else
  int count=__VERIFIER_nondet_int();
  assume_abort_if_not(count>=0);
#if CASE == 20
  assume_abort_if_not(count<=128); // Highest worker value 127 fits signed i8.
#elif CASE == 21
  assume_abort_if_not(count<=129); // Worker value 128 changes signed meaning.
#elif CASE == 22
  assume_abort_if_not(count<=256); // Highest worker value 255 fits unsigned i8.
#elif CASE == 23
  assume_abort_if_not(count<=257); // Worker values 0 and 256 collide after i8.
#else
  assume_abort_if_not(count<=1024);
#endif
#endif
  pthread_t *ids=safe_malloc((size_t)count*sizeof(pthread_t));
#if CASE == 26
  data=shifted_malloc(((size_t)count+1)*sizeof(datum));
#else
  data=safe_malloc(((size_t)count+1)*sizeof(datum));
#endif
#if CASE == 24
  other=data;
#endif
#if CASE == 17
  mutate(&data); // Slot-address escape: intentionally outside narrow contract.
#endif
#if CASE == 3 || CASE == 30 || CASE == 31
  for (long long position=0;position<count;position++) {
#else
  for (int position=0;position<count;position++) {
#endif
#if CASE == 14
    data++;
#endif
#if CASE == 16
    intptr_t token=position/2;
#else
    intptr_t token=position;
#endif
    pthread_create(&ids[position],0,worker,(void*)token);
  }
#if CASE == 18
  data[0]=3;
#endif
  return 0;
}
