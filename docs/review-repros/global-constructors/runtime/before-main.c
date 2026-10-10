#include <assert.h>
int flag=1;
__attribute__((constructor)) static void initialize(void) { flag=0; }
int main(void) { assert(flag==0); return 0; }
