#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#ifdef NATIVE
#include <assert.h>
#endif
#ifndef CASE
#define CASE 0
#endif
#ifdef WIDE
typedef unsigned __int128 value_type;
#else
typedef unsigned int value_type;
#endif
extern int __VERIFIER_nondet_int(void);
extern void *__VERIFIER_nondet_pointer(void);
extern char *__strcpy_chk(char *, const char *, size_t);

static char buffer[16], other[16], source[16] = "Bigshot";
static char *heap;
static struct { char prefix[3], text[16]; } object;
static value_type flag, alternate;
static int payload;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
// On both tested Linux little-endian ABIs, clearing the low address byte
// turns &arena[1] into &arena[0], a valid pointer representation change.
static _Alignas(256) int arena[64];
static int *shared = &arena[1];
static int **slot;

static void *writer(void *arg) {
  (void)arg;
#if CASE == 0
  strcpy(heap, "Bigshot");
#elif CASE == 3 || CASE == 4 || CASE == 7
  pthread_mutex_lock(&mutex);
#if CASE == 7
  value_type *dest = __VERIFIER_nondet_int() ? &flag : &alternate;
#else
  value_type *dest = &flag;
#endif
  strcpy((char *)dest, "\1");
  pthread_mutex_unlock(&mutex);
#elif CASE == 5 || CASE == 15
  payload = 2;
#elif CASE == 6
  strcpy(object.text, "Bigshot");
#elif CASE == 8
  strcpy(__VERIFIER_nondet_pointer(), "Bigshot");
#elif CASE == 9
  payload = 2;
#elif CASE == 10
  strcpy((char *)&shared, "");
#elif CASE == 11
  strcpy((char *)slot, "");
#elif CASE == 12 || CASE == 14
  strcpy(buffer, source);
#elif CASE == 13
  __strcpy_chk(buffer, "Bigshot", sizeof buffer);
#elif CASE == 16
  char text[16];
  for (size_t i = 0; i < sizeof text; ++i) {
    switch (buffer[i]) {
      case '0': text[i] = '0'; break;
      case '1': text[i] = '1'; break;
      case '2': text[i] = '2'; break;
      case '3': text[i] = '3'; break;
      case '4': text[i] = '4'; break;
      case '5': text[i] = '5'; break;
      case '6': text[i] = '6'; break;
      case '7': text[i] = '7'; break;
      case '8': text[i] = '8'; break;
      case '9': text[i] = '9'; break;
      case 'a': text[i] = 'a'; break;
      case 'b': text[i] = 'b'; break;
      case 'c': text[i] = 'c'; break;
      case 'd': text[i] = 'd'; break;
      case 'e': text[i] = 'e'; break;
      case 'f': text[i] = 'f'; break;
      case 'x': text[i] = 'x'; break;
      default: text[i] = 0; break;
    }
    if (text[i] == 0) break;
  }
  void *recovered;
  if (sscanf(text, "%p", &recovered) == 1) *(int *)recovered = 2;
#else
  strcpy(buffer, "Bigshot");
#endif
  return NULL;
}

static void *reader(void *arg) {
  (void)arg;
#if CASE == 3 || CASE == 4 || CASE == 7
  pthread_mutex_lock(&mutex);
  int enabled = flag != 0 || alternate != 0;
#if CASE == 4
  if (enabled) payload = 1;
  pthread_mutex_unlock(&mutex);
#else
  pthread_mutex_unlock(&mutex);
  if (enabled) payload = 1;
#endif
#elif CASE == 2
  if (buffer[7] == 0) payload = 1; // includes the copied terminating NUL
#elif CASE == 6
  if (object.text[7] == 0) payload = 1;
#elif CASE == 10
  if (shared == &arena[0]) payload = 1;
#elif CASE == 11
  if (*slot == &arena[0]) payload = 1;
#elif CASE == 12
  strcpy(other, source); // concurrent readers of a mutable source, no writers
#else
  if (buffer[0] == 'B') payload = 1;
#endif
  return NULL;
}

int main(void) {
  pthread_t w, r;
#if CASE == 16
  int owned = 0;
  char text[16];
  snprintf(text, sizeof text, "%p", (void *)&owned);
  strcpy(buffer, text); // publish a pointer through character/control transport
  pthread_create(&w, NULL, writer, NULL);
#ifdef NATIVE
  pthread_join(w, NULL);
  assert(owned == 2);
#else
  owned = 1;
  pthread_join(w, NULL);
#endif
  return 0;
#elif CASE == 0
  heap = malloc(16);
  if (!heap) return 0;
#elif CASE == 11
  slot = malloc(sizeof *slot);
  if (!slot) return 0;
  *slot = &arena[1];
#endif
  pthread_create(&w, NULL, writer, NULL);
#if CASE == 0 || CASE == 10 || CASE == 11 || CASE == 13
  pthread_join(w, NULL);
#if CASE == 0
  if (heap[0] == 'B') payload = 1;
#elif CASE == 13
  if (buffer[0] == 'B') payload = 1;
#else
  pthread_create(&r, NULL, reader, NULL);
#ifdef NATIVE
  pthread_join(r, NULL);
  assert(payload == 1);
  assert((CASE == 10 ? shared : *slot) == &arena[0]);
#else
  payload = 2;
  pthread_join(r, NULL);
#endif
#endif
#elif CASE == 5 || CASE == 15
#if CASE == 5
  char local[8] = "";
#else
  char local[8];
#endif
  char *result = strcpy(local, "B");
#if CASE == 5
  if (local[0] == 'B') payload = 1;
#else
  if (result != local) payload = 1; // returning the original destination
#endif
  pthread_join(w, NULL);
#elif CASE == 9
  strcpy((char *)&w, "\1"); // legal byte access; do not consume corrupted handle
#elif CASE == 14
  source[0] = 'X'; // conflicts with the library's source Read
  pthread_join(w, NULL);
#else
  pthread_create(&r, NULL, reader, NULL);
#if CASE == 3 || CASE == 7
  payload = 2;
#elif CASE == 4
  pthread_mutex_lock(&mutex);
  payload = 2;
  pthread_mutex_unlock(&mutex);
#endif
  pthread_join(w, NULL);
  pthread_join(r, NULL);
#endif
  return 0;
}
