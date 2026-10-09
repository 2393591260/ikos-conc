#include <pthread.h>
struct Pair { int x; int y; };
/* A conforming provider can return overlapping embedded Pair objects from
   union { struct Pair a; struct { int pad; struct Pair b; } nested; }. */
extern struct Pair *get_left(void);
extern struct Pair *get_right(void);
void *a(void *arg) { get_left()->y=1;return 0; }
void *b(void *arg) { get_right()->x=1;return 0; }
int main(void) {pthread_t x,y;pthread_create(&x,0,a,0);pthread_create(&y,0,b,0);return 0;}
