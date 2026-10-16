// SPDX-License-Identifier: MIT
#include <pthread.h>
#include <stdio.h>
#include <errno.h>
#include <locale.h>
#include <wchar.h>
#ifndef CASE
#define CASE 0
#endif
#ifdef WIDE
typedef long long datum;
#else
typedef int datum;
#endif
#ifdef MAPPED
extern int scan_entry(const char *, const char *, ...) __asm__("sscanf");
#define SCAN scan_entry
#else
#define SCAN sscanf
#endif
static int output;
static datum shared;
static char input[] = "12345";
#if CASE == 24
static pthread_rwlock_t guard = PTHREAD_RWLOCK_INITIALIZER;
#elif CASE == 25
static pthread_mutex_t guard = PTHREAD_MUTEX_INITIALIZER;
#endif
#if CASE == 17
int *escaped_errno;
#endif
#if CASE == 18
extern int external_state;
#endif
#if CASE == 15
int sscanf(const char *source, const char *format, ...) {
  (void)source; (void)format;
  shared = 1;
  return 0;
}
#endif
static void *reader(void *unused) { return output ? unused : 0; }
static void *writer(void *unused) { (void)unused; shared = 1; return 0; }
static void *scanner(void *unused) {
  (void)unused;
#if CASE == 24
  pthread_rwlock_rdlock(&guard);
#elif CASE == 25
  pthread_mutex_lock(&guard);
#endif
#if CASE == 15
  (void)sscanf(input, "%d", &output);
#else
  (void)SCAN(input + (CASE == 22), "%d", &output);
#endif
#if CASE == 24
  pthread_rwlock_unlock(&guard);
#elif CASE == 25
  pthread_mutex_unlock(&guard);
#endif
  return 0;
}
static void *input_writer(void *unused) {
  (void)unused;
#if CASE == 23
  ((unsigned char *)&output)[1] = 3;
#else
  input[CASE == 22 ? 3 : 0] = '3';
#endif
  return 0;
}
int main(void) {
  pthread_t a, b;
#if CASE == 0 || CASE == 16 || CASE == 20
#if CASE == 20
  int local;
  (void)SCAN(input, "%d", &local);
#elif CASE == 16
  int (*operation)(const char *, const char *, ...) = SCAN;
  (void)operation(input, "%d", &output);
#else
  (void)SCAN(input, "%d", &output);
#endif
  pthread_create(&a, 0, reader, 0);
  pthread_create(&b, 0, reader, 0);
#elif CASE == 1 || CASE == 15 || CASE == 24 || CASE == 25
  pthread_create(&a, 0, scanner, 0);
  pthread_create(&b, 0, scanner, 0);
#elif CASE == 2 || CASE == 22 || CASE == 23
  pthread_create(&a, 0, scanner, 0);
  pthread_create(&b, 0, input_writer, 0);
#elif CASE == 3
  int value = 7;
  (void)SCAN("bad", "%d", &value);
  if (value == 7) {
    pthread_create(&a, 0, writer, 0);
    pthread_create(&b, 0, writer, 0);
  }
#elif CASE == 4
  int value = 0;
  if (SCAN("bad", "%d", &value) != 1) {
    pthread_create(&a, 0, writer, 0);
    pthread_create(&b, 0, writer, 0);
  }
#elif CASE == 5
  int *saved_errno = __errno_location();
  *saved_errno = 0;
  (void)SCAN("999999999999999999999999999999999999999999", "%d", &output);
  if (*saved_errno != 0) {
    pthread_create(&a, 0, writer, 0);
    pthread_create(&b, 0, writer, 0);
  }
#elif CASE == 6
  (void)SCAN(input, "%n", &output);
#elif CASE == 7
  char buffer[32];
  (void)SCAN(input, "%s", buffer);
#elif CASE == 8
  char format[] = "%d";
  (void)SCAN(input, format, &output);
#elif CASE == 9
  static char format[] = "%d";
  (void)SCAN(input, format, &output);
#elif CASE == 10
  long long value;
  (void)SCAN(input, "%d", &value);
#elif CASE == 11
  short value;
  (void)SCAN(input, "%d", &value);
#elif CASE == 12
  struct { int first, second; } pair;
  (void)SCAN(input, "%d", &pair.second);
#elif CASE == 13
  (void)SCAN(input, "%d");
#elif CASE == 14
  int second;
  (void)SCAN(input, "%d%d", &output, &second);
#elif CASE == 17
  escaped_errno = __errno_location();
  int private_output;
  (void)SCAN(input, "%d", &private_output);
#elif CASE == 18
  output = external_state;
  int private_output;
  (void)SCAN(input, "%d", &private_output);
#elif CASE == 19
  setlocale(LC_ALL, "C");
  (void)SCAN(input, "%d", &output);
#elif CASE == 21
  int *saved_errno = __errno_location();
  *saved_errno = 0;
  wchar_t value;
  (void)SCAN("\xff", "%lc", &value);
  if (*saved_errno != 0) {
    pthread_create(&a, 0, writer, 0);
    pthread_create(&b, 0, writer, 0);
  }
#endif
#if CASE >= 6 && CASE != 15 && CASE != 16 && CASE != 20 && CASE != 21 && CASE < 22
  pthread_create(&a, 0, reader, 0);
  pthread_create(&b, 0, reader, 0);
#endif
  return 0;
}
