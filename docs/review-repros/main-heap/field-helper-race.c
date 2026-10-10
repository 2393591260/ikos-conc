#include <pthread.h>
#include <stdlib.h>
struct Cell { char padding[5]; int value; };
struct Cell *different_name;
void write_helper(struct Cell *object) {object->value=3;}
void *worker(void *argument) {write_helper(different_name);return 0;}
int main(void) {
 pthread_t thread;
 different_name=malloc(sizeof(struct Cell));
 pthread_create(&thread,0,worker,0);
 write_helper(different_name);
 pthread_join(thread,0);return 0;
}
