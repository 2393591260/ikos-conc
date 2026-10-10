#include <pthread.h>
#include <stdlib.h>
#ifdef NATIVE
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#endif
#ifndef CONTROL
#define CONTROL 0
#endif
// This fixture intentionally uses the Linux/GNU packed layout, not portable ABI assumptions.
static int before __attribute__((aligned(256)));
static int data;
union slots { int *first; struct __attribute__((packed)) { unsigned char byte; int *second; } shifted; };
static union slots *shared;
static void *writer(void *arg) {
  (void)arg;
#if !CONTROL
  shared->shifted.second = NULL;
#endif
  return NULL;
}
static void *reader(void *arg) {
  (void)arg;
  if (shared->first == NULL) data = 1;
  return NULL;
}
int main(void) {
  pthread_t w, r;
  shared = malloc(sizeof *shared);
  if (!shared) return 0;
  shared->first = &before;
  pthread_create(&w, NULL, writer, NULL); pthread_join(w, NULL);
  pthread_create(&r, NULL, reader, NULL);
#ifdef NATIVE
  pthread_join(r, NULL);
  assert(((uintptr_t)&before & 255u) == 0);
  assert(data == (CONTROL ? 0 : 1));
  printf("packed overlap guard reached=%d; width=%zu\n", data, sizeof(int *));
#else
  data = 2; pthread_join(r, NULL);
#endif
  return 0;
}
