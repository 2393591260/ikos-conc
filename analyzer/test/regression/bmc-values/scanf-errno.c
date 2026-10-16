// SPDX-License-Identifier: MIT
#include <pthread.h>
#include <stdio.h>
#include <wchar.h>
#include <errno.h>
#ifndef KIND
#define KIND 0
#endif
#ifndef CASE
#define CASE 0
#endif
#ifdef WIDE
typedef long long datum;
#else
typedef int datum;
#endif
#ifdef MAPPED
#if KIND == 0
extern int scan_entry(const char *, const char *, ...) __asm__("sscanf");
#elif KIND == 1
extern int scan_entry(const char *, ...) __asm__("scanf");
#else
extern int scan_entry(FILE *, const char *, ...) __asm__("fscanf");
#endif
#else
#if KIND == 0
#define scan_entry sscanf
#elif KIND == 1
#define scan_entry scanf
#else
#define scan_entry fscanf
#endif
#endif
static datum shared;
static void *writer(void *unused) { (void)unused; shared = 1; return 0; }
int main(void) {
  int *saved_errno = __errno_location();
  *saved_errno = 0;
#if CASE != 2
  wchar_t value;
#if KIND == 0
  (void)scan_entry("\xff", "%lc", &value);
#elif KIND == 1
  (void)scan_entry("%lc", &value);
#else
  (void)scan_entry(stdin, "%lc", &value);
#endif
#endif
#if CASE != 1
  if (*saved_errno != 0)
#endif
  {
    pthread_t first, second;
    pthread_create(&first, 0, writer, 0);
    pthread_create(&second, 0, writer, 0);
  }
  return 0;
}
