#include <assert.h>
#include <limits.h>
#include <stdint.h>
int main(void) {
  assert(CHAR_BIT==8 && sizeof(short)==2 && sizeof(unsigned long long)==8);
  uint16_t endian=1;
  assert(*(unsigned char *)&endian==1); /* Local x86 ILP32/LP64 fixture. */
  short signed_value=0;
  *((signed char *)&signed_value+1)=-1;
  assert(signed_value==-256);
  unsigned long long wide=0;
  *((unsigned char *)&wide+7)=255;
  assert(wide==0xff00000000000000ULL);
  unsigned short word=511;
  assert(*(unsigned char *)&word==255);
  word=257;
  *(unsigned char *)&word=1;
  assert(word==257 && word!=256);
  return 0;
}
