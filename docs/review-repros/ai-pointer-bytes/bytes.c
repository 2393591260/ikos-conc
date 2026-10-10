#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#ifdef NATIVE
#include <assert.h>
#include <stdio.h>
#endif
#ifndef CASE
#define CASE 0
#endif
#ifndef CONTROL
#define CONTROL 0
#endif
static int before, after, data;
struct object { int *pointer; unsigned char neighbor[sizeof(int *)]; };
static int *shared = &before;
static struct object global_object;
static struct object *heap_object;
static int **slot(void) {
#if CASE == 1 || CASE == 8
  return &global_object.pointer;
#elif CASE == 2 || CASE == 3
  return &heap_object->pointer;
#else
  return &shared;
#endif
}
static void *writer(void *arg) {
  (void)arg;
#if CASE == 6
  int *desired = CONTROL ? &before : NULL;
#else
  int *desired = CONTROL ? &before : &after;
#endif
#if CASE == 4
  *slot() = desired;
#elif CASE == 5
  memcpy(slot(), &desired, sizeof desired);
#else
  const unsigned char *source = (const unsigned char *)&desired;
  volatile unsigned char *dest = (volatile unsigned char *)slot();
#if CASE == 3
  dest = heap_object->neighbor;
#elif CASE == 8
  dest = global_object.neighbor;
#endif
  for (size_t i = 0; i < sizeof desired; i++) dest[i] = source[i];
#endif
  return NULL;
}
static void *reader(void *arg) {
  (void)arg;
#if CASE == 6
  if (*slot() == NULL) data = 1;
#else
  if (*slot() == &after) data = 1;
#endif
  return NULL;
}
int main(void) {
  pthread_t w, r;
#if CASE == 2 || CASE == 3
  heap_object = malloc(sizeof *heap_object);
  if (!heap_object) return 0;
#endif
  *slot() = &before;
  pthread_create(&w, NULL, writer, NULL);
  pthread_join(w, NULL);
  pthread_create(&r, NULL, reader, NULL);
#ifdef NATIVE
  pthread_join(r, NULL);
  assert(data == ((CONTROL || CASE == 3 || CASE == 8) ? 0 : 1));
  printf("pointer guard reached=%d; pointer width=%zu\n", data, sizeof(int *));
#else
  data = 2;
  pthread_join(r, NULL);
#endif
  return 0;
}
