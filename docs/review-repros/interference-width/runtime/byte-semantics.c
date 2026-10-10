#include <assert.h>
#include <stdio.h>
int main(void) {
  short high = 0;
  *((signed char *)&high + 1) = 1;
  assert(high == 256);
  short low = 0;
  *(signed char *)&low = -1;
  assert(low == 255);
  short combined = 256;
  *(signed char *)&combined = -1;
  assert(combined == 511);
  printf("high=%d low=%d combined=%d\n", high, low, combined);
}
