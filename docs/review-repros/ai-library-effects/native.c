/* Check the library value effects used in the concurrent counterexamples.
 * This sequential probe is not a race witness or a validator. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
int main(void) {
  int channel[2], input=1, flag=0;
  assert(pipe(channel)==0);
  assert(write(channel[1],&input,sizeof input)==sizeof input);
  assert(read(channel[0],&flag,sizeof flag)==sizeof flag);
  assert(flag==1);
  assert(read(channel[0],0,0)==0);
  flag=0;
  assert(sscanf("1","%d",&flag)==1 && flag==1);
  flag=0;
  assert(printf("x%n",&flag)==1 && flag==1);
  int count=0; char buffer[4];
  assert(sprintf(buffer,"x%n",&count)==1 && count==1);
  count=0;
  assert(snprintf(0,0,"x%n",&count)==1 && count==1);
  count=0;
  assert(snprintf(buffer,sizeof buffer,"x%n",&count)==1 && count==1);
  unsigned char byte=0;
  strncpy((char *)&byte,"\1",1);
  assert(byte==1);
  puts(" library value effects and zero-byte continuation checked");
  close(channel[0]);close(channel[1]);
}
