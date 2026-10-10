#include <pthread.h>
#include <stdlib.h>
struct node { struct node *next; int value; };
static struct node *published;
static void *worker(void *arg) { ((struct node *)arg)->value=3; return 0; }
int main(void) { struct node *p=malloc(sizeof(*p)), *q=malloc(sizeof(*q)); if (!p || !q) return 0; p->next=q; p->value=0; q->next=0; q->value=0; published=p; pthread_t unused; /* q published directly through arg */ pthread_t t; pthread_create(&t,0,worker,q); q->value=2; pthread_join(t,0); return 0; }
