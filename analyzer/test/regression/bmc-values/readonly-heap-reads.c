// SPDX-License-Identifier: MIT
#include <pthread.h>
#include <stdlib.h>
#ifndef CASE
#define CASE 0
#endif
#ifdef WIDE
typedef long long datum;
extern long long __VERIFIER_nondet_longlong(void);
#define arbitrary __VERIFIER_nondet_longlong
#else
typedef int datum;
extern int __VERIFIER_nondet_int(void);
#define arbitrary __VERIFIER_nondet_int
#endif
extern int __VERIFIER_nondet_int(void);
extern void __VERIFIER_atomic_begin(void);
extern void __VERIFIER_atomic_end(void);
static void guard(int condition) { if (!condition) abort(); }
static datum *left, *right, **matrix;
static int count, rows, columns;
static datum result;

static datum *make_vector(int length) {
  guard(length >= 0);
  guard((unsigned)length <= 4294967295u / sizeof(datum));
#if CASE == 5
  return &result;
#else
  datum *data = malloc(sizeof(datum) * (unsigned)length - (CASE == 11));
#if CASE == 17
  if (__VERIFIER_nondet_int()) return data;
#endif
#if CASE == 12
  for (int position = 0; position < length - 1; ++position)
#else
  for (int position = 0; position < length; ++position)
#endif
    data[position] = arbitrary();
#if CASE == 38
  free(data);
#endif
  return data;
#endif
}

static void mutate(datum *data) { data[0] = 0; }
static void *reader(void *ignored) {
  (void)ignored;
#if CASE == 45
  int *destination;
  if (__VERIFIER_nondet_int()) destination = &rows;
  else destination = &columns;
  *destination = 0;
#endif
  for (int position = 0; position < count; ++position) {
#if CASE == 2
    left[position] = 0;
#elif CASE == 21
    matrix[0][0] = 0;
#elif CASE == 35
    mutate(left);
#endif
#if CASE != 6
#if CASE == 25
    guard(((volatile datum *)left)[position] >= 0 &&
          ((volatile datum *)left)[position] < rows);
#else
    guard(left[position] >= 0 && left[position] < rows);
#endif
    guard(right[position] >= 0 && right[position] < columns);
#endif
#if CASE != 7
    __VERIFIER_atomic_begin();
#endif
#if CASE == 3
    result = matrix[left[position + 1]][right[position]];
#elif CASE == 34
    result = matrix[left[position]][right[position] + columns];
#else
    result = matrix[left[position]][right[position]];
#endif
#if CASE != 7
    __VERIFIER_atomic_end();
#endif
#if CASE == 15
    matrix[0] = &result;
#endif
  }
  return 0;
}

int main(void) {
  pthread_t one, two;
  count = __VERIFIER_nondet_int();
  left = make_vector(count);
#if CASE == 33
  right = left;
#else
  right = make_vector(count + (CASE == 24));
#endif
  rows = __VERIFIER_nondet_int();
  columns = __VERIFIER_nondet_int();
  guard((unsigned)rows <= 4294967295u / sizeof(datum *));
#if CASE == 47
  guard(count >= 1);
  int checked = __VERIFIER_nondet_int();
  guard(checked >= 0 && checked < count);
  guard(left[checked] == 0);
  rows = count;
  matrix = (datum **)left;
#else
  matrix = malloc(sizeof(datum *) * (unsigned)rows - (CASE == 36));
#endif
#if CASE == 41
  pthread_create(&one, 0, reader, 0);
#endif
  for (int row = CASE == 18 ? 1 : 0;
       CASE == 13 ? row <= rows : row < rows;
       row += CASE == 1 ? 2 : 1) {
#if CASE == 19
    if (row == 0) continue;
#endif
#if CASE == 4
    matrix[row] = &result;
#elif CASE == 23
    matrix[row] = 0;
#elif CASE == 29
    matrix[row] = (datum *)&one;
#elif CASE == 37
    matrix[row] = (datum *)(unsigned)__VERIFIER_nondet_int();
#elif CASE == 39
    matrix[row] = row ? make_vector(columns) : &result;
#elif CASE == 46
    datum *made = make_vector(columns);
    pthread_create(&one, 0, reader, 0);
    matrix[row] = made;
#else
    matrix[row] = make_vector(columns);
#endif
#if CASE == 43
    --columns;
#endif
  }
#if CASE == 8
  ((char *)matrix)[0] = 0;
#elif CASE == 9
  extern void opaque(void *);
  opaque(matrix);
#elif CASE == 16
  ((char *)left)[0] = 0;
#elif CASE == 22
  matrix = 0;
#endif
#if CASE != 41
  pthread_create(&one, 0, reader, 0);
#endif
  pthread_create(&two, 0, reader, 0);
#if CASE == 10
  rows = 0;
#elif CASE == 14
  columns = 0;
#elif CASE == 31
  count = 2;
#elif CASE == 42
  matrix[0] = &result;
#endif
#if CASE == 28
  pthread_join(two, 0);
#else
  pthread_join(one, 0);
#endif
#if CASE != 27
  pthread_join(two, 0);
#endif
  return (int)result;
}
