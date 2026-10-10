#include <assert.h>
#include <stddef.h>
#include <stdio.h>
extern void abi_store(size_t,void *,void *,int) __asm__("__atomic_store");
int main(void) {
  unsigned long long flag=0, one=1;
  abi_store(sizeof flag,&flag,&one,__ATOMIC_SEQ_CST);
  assert(flag==1);
  printf("%zu-byte generic atomic store publishes 1\n",sizeof flag);
#if defined(__SIZEOF_INT128__)
  __int128 wide=0, wide_one=1;
  abi_store(sizeof wide,&wide,&wide_one,__ATOMIC_SEQ_CST);
  assert(wide==1);
  printf("%zu-byte generic atomic store publishes 1\n",sizeof wide);
#endif
}
