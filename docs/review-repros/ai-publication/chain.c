#include <pthread.h>
#include <stdlib.h>
#include <stdint.h>
#ifndef CASE
#define CASE 0
#endif
#if CASE == 1
#define worker renamed_worker
#define published renamed_root
#endif
#if CASE == 13
typedef int64_t value_type;
#else
typedef int value_type;
#endif
struct node { struct node *next; value_type value; };
static struct node *published;
#if CASE == 9
static _Thread_local struct node tls_leaf;
#endif
static void attach(struct node *root, struct node *leaf) { root->next = leaf; }
static void *worker(void *arg) {
#if CASE == 7
  struct node *private_node = malloc(sizeof(*private_node));
  if (private_node) private_node->value = 3;
#elif CASE == 2
  published->next->next->value = 3;
#else
  published->next->value = 3;
#endif
  return 0;
}
int main(void) {
#if CASE == 10 || CASE == 11
  struct node stack_root;
  struct node *p = &stack_root;
#else
  struct node *p = malloc(sizeof(*p));
#endif
#if CASE == 8 || CASE == 11
  struct node stack_leaf;
  struct node *q = &stack_leaf;
#elif CASE == 9
  struct node *q = &tls_leaf;
#else
  struct node *q = malloc(sizeof(*q));
#endif
  if (!p || !q) return 0;
  p->value = 0; q->value = 0;
#if CASE == 2
  struct node *middle = malloc(sizeof(*middle));
  if (!middle) return 0;
  middle->next = q; p->next = middle;
#elif CASE == 5
  attach(p, q);
#elif CASE == 12
  struct node *slots[1];
  slots[0] = q;
  attach(p, slots[0]);
#elif CASE == 4
  published = p; p->next = q;
#else
  p->next = q;
#endif
#if CASE == 3
  q->next = p;
#else
  q->next = 0;
#endif
  published = p;
  pthread_t t;
  pthread_create(&t, 0, worker, 0);
#if CASE == 6
  pthread_join(t, 0);
#endif
  q->value = 2;
#if CASE != 6
  pthread_join(t, 0);
#endif
  return 0;
}
