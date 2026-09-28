/* 测试目标: Unknown 触发边界 —— 同一内存单元一侧原子写、另一侧普通写（经指针转型），
 *           触发 phase-3「原子/非原子混合」降级为 Unknown
 * 预期结果: Unknown
 */
#include <pthread.h>
#include <stdatomic.h>
_Atomic int glob;
int *p;
void *t(void *arg) { atomic_store(&glob, 1); return 0; }   /* 原子写 */
void *u(void *arg) { *p = 0; return 0; }                    /* 普通写（经指针） */
int main(void) {
  pthread_t a, b;
  p = (int *)&glob;
  pthread_create(&a, 0, t, 0);
  pthread_create(&b, 0, u, 0);
  pthread_join(a, 0); pthread_join(b, 0);
  return 0;
}
