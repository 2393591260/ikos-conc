#include <pthread.h>
#include <stdlib.h>
#ifndef CASE
#define CASE 0
#endif

struct node { int value; struct node *next; };
static struct node *root;
static struct node *mirror;
static pthread_mutex_t protection = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t other = PTHREAD_MUTEX_INITIALIZER;
static volatile int observed;
extern void *__VERIFIER_nondet_pointer(void);
extern int __VERIFIER_nondet_int(void);

static void initialize(struct node *p, int value) {
  p->value = value;
  p->next = 0;
}
static void leak(struct node *p) { mirror = p; }
static void *worker(void *unused) {
  (void)unused;
#if CASE == 8
  for (int i = 0; i < 2; ++i) {
#endif
  struct node *p = malloc(sizeof(*p));
#if CASE == 1
  pthread_mutex_lock(&protection);
  root->next = p;
  pthread_mutex_unlock(&protection);
  initialize(p, 7);
#else
#if CASE == 10
  leak(p);
#endif
  initialize(p, 7);
#if CASE == 6
  mirror = p;
#endif
#if CASE != 2
  pthread_mutex_lock(&protection);
#endif
  root->next = p;
#if CASE == 4
  root->next = __VERIFIER_nondet_pointer();
#elif CASE == 5
  // Same-byte write is harmless concretely but needs a representation proof.
  unsigned char *bytes = (unsigned char *)&root->next;
  unsigned char saved = *bytes;
  *bytes = saved;
#endif
#if CASE != 2
  pthread_mutex_unlock(&protection);
#endif
#if CASE == 3
  initialize(p, 9);
#endif
#endif
#if CASE == 8
  }
#endif
  return 0;
}

int main(void) {
  pthread_t thread;
  struct node *initial = malloc(sizeof(*initial));
  root = malloc(sizeof(*root));
  if (!initial || !root) return 0;
  initialize(initial, 3);
  initialize(root, 0);
  root->next = initial;
  mirror = initial;
  pthread_create(&thread, 0, worker, 0);
#if CASE == 7
  pthread_mutex_lock(&other);
#else
  pthread_mutex_lock(&protection);
#endif
#if CASE == 6 || CASE == 10
  struct node *selected = mirror;
#else
  struct node *selected = root->next;
#endif
  if (selected) observed = selected->value;
#if CASE == 7
  pthread_mutex_unlock(&other);
#else
  pthread_mutex_unlock(&protection);
#endif
  pthread_join(thread, 0);
#if CASE == 9
  free(root);
#endif
  return 0;
}
