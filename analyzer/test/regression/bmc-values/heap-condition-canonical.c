// SPDX-License-Identifier: MIT
// Real cond_wait markers precede malloc in main, which is target trace b.
#define _GNU_SOURCE
#include <pthread.h>
#include <stdlib.h>
#ifndef MODE
#define MODE 0
#endif
#ifdef WIDE
typedef long long element;
#else
typedef int element;
#endif
#if MODE == 2
static pthread_mutex_t mutex;
static pthread_mutexattr_t attributes;
#else
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
#endif
static pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
static element shared;

// Post-loop root checks sort by name. This root precedes main, and the runner
// additionally requires the actual exported a/b entries, without relabeling.
static void *aaa_worker(void *argument) {
  element *p = argument;
#if MODE == 2
  element value = *p;
  shared = value;
#else
  *p = 1;
#if MODE == 1
  shared = 2;
#endif
#endif
  return 0;
}

int main(void) {
#if MODE == 2
  pthread_mutexattr_init(&attributes);
  pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_ERRORCHECK);
  pthread_mutex_init(&mutex, &attributes);
  pthread_mutexattr_destroy(&attributes);
#endif
  pthread_mutex_lock(&mutex);
  // The finite execution chooses a POSIX spurious wake. Both actual synthetic
  // release/reacquire events must be present before the allocation occurrence.
  pthread_cond_wait(&condition, &mutex);
  pthread_mutex_unlock(&mutex);
  element *p = malloc(sizeof(element));
  if (!p) abort();
  *p = 7;
  pthread_t child;
  pthread_create(&child, 0, aaa_worker, p);
#if MODE == 0
  *p = 2;
#else
  shared = 1;
#endif
  pthread_join(child, 0);
  return 0;
}
