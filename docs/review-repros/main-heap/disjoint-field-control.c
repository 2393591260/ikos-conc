#include <pthread.h>
#include <stdlib.h>
struct Cell {int left;int right;};
struct Cell *pointer;
void *worker(void *argument) {pointer->left=3;return 0;}
int main(void) {
 pthread_t thread;pointer=malloc(sizeof(struct Cell));
 pthread_create(&thread,0,worker,0);
 pointer->right=4;
 pthread_join(thread,0);return 0;
}
