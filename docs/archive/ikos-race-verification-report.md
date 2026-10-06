# IKOS 并发分析边界验证报告

> 本文档为逐用例的完整复现：每个用例单独跑一次，记录所用指令与完整终端输出。

- **IKOS 二进制**: `/home/ruan/ikos/install/bin/ikos`
- **仓库根**: `/home/ruan/ikos`
- **分析选项**: `--analyses=race --concurrency=auto`

## 用例总览

| # | 用例 | 层级 | 预期 | 实际 |
|---|------|------|------|------|
| 1 | `create-race.c` | L1 | Race | Race |
| 2 | `create-safe.c` | L1 | Safe | Safe |
| 3 | `join-race.c` | L1 | Race | Race |
| 4 | `join-safe.c` | L1 | Safe | Safe |
| 5 | `mutex-race.c` | L1 | Race | Race |
| 6 | `mutex-safe.c` | L1 | Safe | Safe |
| 7 | `rwlock-race.c` | L1 | Race | Race |
| 8 | `rwlock-safe.c` | L1 | Safe | Safe |
| 9 | `spin-race.c` | L1 | Race | Race |
| 10 | `spin-safe.c` | L1 | Safe | Safe |
| 11 | `cond-mutex-race.c` | L2 | Race | Race |
| 12 | `cond-nohb.c` | L2 | Race | Race |
| 13 | `pseudo-lock-distinct.c` | L2 | Race | Race |
| 14 | `pseudo-lock-shared.c` | L2 | Safe | Safe |
| 15 | `unknown-mixed-atomic.c` | L2 | Unknown | Unknown |
| 16 | `unknown-pointer-alias.c` | L2 | Race | Race |

---

## 用例 1：`L1/create-race.c`

**预期结果**: Race　|　**实际结果**: Race

### 源码

```c
/* 测试目标: pthread_create —— main 在创建线程【后】写共享变量，与子线程读并发
 * 预期结果: Race
 */
#include <pthread.h>
int x = 0;
void *worker(void *arg) { int r = x; (void)r; return 0; }
int main(void) {
  pthread_t t;
  pthread_create(&t, 0, worker, 0);
  x = 1;                       /* 写发生在 create 之后 → 与 worker 读并发 */
  pthread_join(t, 0);
  return 0;
}
```

### 运行指令

```bash
$ /home/ruan/ikos/install/bin/ikos --analyses=race --concurrency=auto -o /tmp/rep_L1_create-race.db test/L1/create-race.c
```

### 终端输出

```text
[*] Compiling test/L1/create-race.c
[*] Running ikos preprocessor
[*] Running ikos analyzer
[*] Translating LLVM bitcode to AR
Warning: llvm function pthread_join and ar intrinsic ar.pthread.join have a different type
LLVM function declaration
ikos ar expected function declaration
Expected signature will be ignored.
[*] Running liveness analysis
[*] Running widening hint analysis
[*] Running interprocedural value analysis
[*] Race analysis requested: auto-enabling thread-modular transfer-function gates
[*] Running function pointer analysis (thread-modular)
[*] Starting global Worklist fixpoint iteration
[*] Analyzing function 'main'
[*] Analyzing function 'worker'
[*] Analyzing function 'main'
[*] Running post-loop checks for all analyzed functions
[*] Checking properties for function 'worker'
[*] Checking properties for function 'main'

# Time stats:
clang        : 0.052 sec
ikos-analyzer: 0.009 sec
ikos-pp      : 0.005 sec

# Summary:
Total number of checks                : 1
Total number of unreachable checks    : 0
Total number of safe checks           : 0
Total number of definite unsafe checks: 1
Total number of warnings              : 0
Total number of unknown checks        : 0

The program is definitely UNSAFE

# Results
test/L1/create-race.c: In function 'main':
test/L1/create-race.c:10:5: error: potential data race: conflicting memory access without a common lock
  x = 1;                       /* 写发生在 create 之后 → 与 worker 读并发 */
    ^
```

---

## 用例 2：`L1/create-safe.c`

**预期结果**: Safe　|　**实际结果**: Safe

### 源码

```c
/* 测试目标: pthread_create 的 create-edge happens-before —— main 在创建线程【前】写共享变量，子线程读
 * 预期结果: Safe
 */
#include <pthread.h>
int x = 0;
void *worker(void *arg) { int r = x; (void)r; return 0; }
int main(void) {
  x = 1;                       /* 写发生在 create 之前 → HB 子线程读 */
  pthread_t t;
  pthread_create(&t, 0, worker, 0);
  pthread_join(t, 0);
  return 0;
}
```

### 运行指令

```bash
$ /home/ruan/ikos/install/bin/ikos --analyses=race --concurrency=auto -o /tmp/rep_L1_create-safe.db test/L1/create-safe.c
```

### 终端输出

```text
[*] Compiling test/L1/create-safe.c
[*] Running ikos preprocessor
[*] Running ikos analyzer
[*] Translating LLVM bitcode to AR
Warning: llvm function pthread_join and ar intrinsic ar.pthread.join have a different type
LLVM function declaration
ikos ar expected function declaration
Expected signature will be ignored.
[*] Running liveness analysis
[*] Running widening hint analysis
[*] Running interprocedural value analysis
[*] Race analysis requested: auto-enabling thread-modular transfer-function gates
[*] Running function pointer analysis (thread-modular)
[*] Starting global Worklist fixpoint iteration
[*] Analyzing function 'main'
[*] Analyzing function 'worker'
[*] Analyzing function 'main'
[*] Running post-loop checks for all analyzed functions
[*] Checking properties for function 'worker'
[*] Checking properties for function 'main'

# Time stats:
clang        : 0.051 sec
ikos-analyzer: 0.009 sec
ikos-pp      : 0.005 sec

# Summary:
Total number of checks                : 0
Total number of unreachable checks    : 0
Total number of safe checks           : 0
Total number of definite unsafe checks: 0
Total number of warnings              : 0
Total number of unknown checks        : 0

The program is SAFE

# Results
No entries.
```

---

## 用例 3：`L1/join-race.c`

**预期结果**: Race　|　**实际结果**: Race

### 源码

```c
/* 测试目标: pthread_join —— main 在 join【前】读共享变量，与子线程写并发
 * 预期结果: Race
 */
#include <pthread.h>
int x = 0;
void *worker(void *arg) { x = 1; return 0; }
int main(void) {
  pthread_t t;
  pthread_create(&t, 0, worker, 0);
  int r = x; (void)r;          /* 未 join 就读 → 与 worker 写并发 */
  pthread_join(t, 0);
  return 0;
}
```

### 运行指令

```bash
$ /home/ruan/ikos/install/bin/ikos --analyses=race --concurrency=auto -o /tmp/rep_L1_join-race.db test/L1/join-race.c
```

### 终端输出

```text
[*] Compiling test/L1/join-race.c
[*] Running ikos preprocessor
[*] Running ikos analyzer
[*] Translating LLVM bitcode to AR
Warning: llvm function pthread_join and ar intrinsic ar.pthread.join have a different type
LLVM function declaration
ikos ar expected function declaration
Expected signature will be ignored.
[*] Running liveness analysis
[*] Running widening hint analysis
[*] Running interprocedural value analysis
[*] Race analysis requested: auto-enabling thread-modular transfer-function gates
[*] Running function pointer analysis (thread-modular)
[*] Starting global Worklist fixpoint iteration
[*] Analyzing function 'main'
[*] Analyzing function 'worker'
[*] Analyzing function 'main'
[*] Analyzing function 'worker'
[*] Analyzing function 'main'
[*] Running post-loop checks for all analyzed functions
[*] Checking properties for function 'worker'
[*] Checking properties for function 'main'

# Time stats:
clang        : 0.048 sec
ikos-analyzer: 0.010 sec
ikos-pp      : 0.005 sec

# Summary:
Total number of checks                : 1
Total number of unreachable checks    : 0
Total number of safe checks           : 0
Total number of definite unsafe checks: 1
Total number of warnings              : 0
Total number of unknown checks        : 0

The program is definitely UNSAFE

# Results
test/L1/join-race.c: In function 'worker':
test/L1/join-race.c:6:29: error: potential data race: conflicting memory access without a common lock
void *worker(void *arg) { x = 1; return 0; }
                            ^
```

---

## 用例 4：`L1/join-safe.c`

**预期结果**: Safe　|　**实际结果**: Safe

### 源码

```c
/* 测试目标: pthread_join 的 join-edge happens-before —— 子线程写，main join 后再读
 * 预期结果: Safe
 */
#include <pthread.h>
int x = 0;
void *worker(void *arg) { x = 1; return 0; }
int main(void) {
  pthread_t t;
  pthread_create(&t, 0, worker, 0);
  pthread_join(t, 0);          /* join → worker 写 HB main 读 */
  int r = x; (void)r;
  return 0;
}
```

### 运行指令

```bash
$ /home/ruan/ikos/install/bin/ikos --analyses=race --concurrency=auto -o /tmp/rep_L1_join-safe.db test/L1/join-safe.c
```

### 终端输出

```text
[*] Compiling test/L1/join-safe.c
[*] Running ikos preprocessor
[*] Running ikos analyzer
[*] Translating LLVM bitcode to AR
Warning: llvm function pthread_join and ar intrinsic ar.pthread.join have a different type
LLVM function declaration
ikos ar expected function declaration
Expected signature will be ignored.
[*] Running liveness analysis
[*] Running widening hint analysis
[*] Running interprocedural value analysis
[*] Race analysis requested: auto-enabling thread-modular transfer-function gates
[*] Running function pointer analysis (thread-modular)
[*] Starting global Worklist fixpoint iteration
[*] Analyzing function 'main'
[*] Analyzing function 'worker'
[*] Analyzing function 'main'
[*] Analyzing function 'worker'
[*] Analyzing function 'main'
[*] Running post-loop checks for all analyzed functions
[*] Checking properties for function 'worker'
[*] Checking properties for function 'main'

# Time stats:
clang        : 0.050 sec
ikos-analyzer: 0.010 sec
ikos-pp      : 0.005 sec

# Summary:
Total number of checks                : 0
Total number of unreachable checks    : 0
Total number of safe checks           : 0
Total number of definite unsafe checks: 0
Total number of warnings              : 0
Total number of unknown checks        : 0

The program is SAFE

# Results
No entries.
```

---

## 用例 5：`L1/mutex-race.c`

**预期结果**: Race　|　**实际结果**: Race

### 源码

```c
/* 测试目标: pthread_mutex_lock/unlock —— 两线程用【不同】互斥锁保护同一共享变量
 * 预期结果: Race
 */
#include <pthread.h>
int x = 0;
pthread_mutex_t m1 = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t m2 = PTHREAD_MUTEX_INITIALIZER;
void *worker(void *arg) { pthread_mutex_lock(&m1); x++; pthread_mutex_unlock(&m1); return 0; }
int main(void) {
  pthread_t t;
  pthread_create(&t, 0, worker, 0);
  pthread_mutex_lock(&m2); x++; pthread_mutex_unlock(&m2);   /* m2 != m1 → 无公共锁 */
  pthread_join(t, 0);
  return 0;
}
```

### 运行指令

```bash
$ /home/ruan/ikos/install/bin/ikos --analyses=race --concurrency=auto -o /tmp/rep_L1_mutex-race.db test/L1/mutex-race.c
```

### 终端输出

```text
[*] Compiling test/L1/mutex-race.c
[*] Running ikos preprocessor
[*] Running ikos analyzer
[*] Translating LLVM bitcode to AR
Warning: llvm function pthread_join and ar intrinsic ar.pthread.join have a different type
LLVM function declaration
ikos ar expected function declaration
Expected signature will be ignored.
[*] Running liveness analysis
[*] Running widening hint analysis
[*] Running interprocedural value analysis
[*] Race analysis requested: auto-enabling thread-modular transfer-function gates
[*] Running function pointer analysis (thread-modular)
[*] Starting global Worklist fixpoint iteration
[*] Analyzing function 'main'
[*] Analyzing function 'worker'
[*] Analyzing function 'main'
[*] Analyzing function 'worker'
[*] Analyzing function 'main'
[*] Running post-loop checks for all analyzed functions
[*] Checking properties for function 'worker'
[*] Checking properties for function 'main'

# Time stats:
clang        : 0.050 sec
ikos-analyzer: 0.013 sec
ikos-pp      : 0.005 sec

# Summary:
Total number of checks                : 1
Total number of unreachable checks    : 0
Total number of safe checks           : 0
Total number of definite unsafe checks: 1
Total number of warnings              : 0
Total number of unknown checks        : 0

The program is definitely UNSAFE

# Results
test/L1/mutex-race.c: In function 'main':
test/L1/mutex-race.c:12:29: error: potential data race: conflicting memory access without a common lock
  pthread_mutex_lock(&m2); x++; pthread_mutex_unlock(&m2);   /* m2 != m1 → 无公共锁 */
                            ^
```

---

## 用例 6：`L1/mutex-safe.c`

**预期结果**: Safe　|　**实际结果**: Safe

### 源码

```c
/* 测试目标: pthread_mutex_lock/unlock —— 两线程用【同一把】互斥锁保护共享变量
 * 预期结果: Safe
 */
#include <pthread.h>
int x = 0;
pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
void *worker(void *arg) { pthread_mutex_lock(&m); x++; pthread_mutex_unlock(&m); return 0; }
int main(void) {
  pthread_t t;
  pthread_create(&t, 0, worker, 0);
  pthread_mutex_lock(&m); x++; pthread_mutex_unlock(&m);
  pthread_join(t, 0);
  return 0;
}
```

### 运行指令

```bash
$ /home/ruan/ikos/install/bin/ikos --analyses=race --concurrency=auto -o /tmp/rep_L1_mutex-safe.db test/L1/mutex-safe.c
```

### 终端输出

```text
[*] Compiling test/L1/mutex-safe.c
[*] Running ikos preprocessor
[*] Running ikos analyzer
[*] Translating LLVM bitcode to AR
Warning: llvm function pthread_join and ar intrinsic ar.pthread.join have a different type
LLVM function declaration
ikos ar expected function declaration
Expected signature will be ignored.
[*] Running liveness analysis
[*] Running widening hint analysis
[*] Running interprocedural value analysis
[*] Race analysis requested: auto-enabling thread-modular transfer-function gates
[*] Running function pointer analysis (thread-modular)
[*] Starting global Worklist fixpoint iteration
[*] Analyzing function 'main'
[*] Analyzing function 'worker'
[*] Analyzing function 'main'
[*] Running post-loop checks for all analyzed functions
[*] Checking properties for function 'worker'
[*] Checking properties for function 'main'

# Time stats:
clang        : 0.047 sec
ikos-analyzer: 0.011 sec
ikos-pp      : 0.005 sec

# Summary:
Total number of checks                : 0
Total number of unreachable checks    : 0
Total number of safe checks           : 0
Total number of definite unsafe checks: 0
Total number of warnings              : 0
Total number of unknown checks        : 0

The program is SAFE

# Results
No entries.
```

---

## 用例 7：`L1/rwlock-race.c`

**预期结果**: Race　|　**实际结果**: Race

### 源码

```c
/* 测试目标: pthread_rwlock_rdlock —— 读锁是共享的（不互斥），两线程都在 rdlock 下【写】共享变量
 * 预期结果: Race
 */
#include <pthread.h>
int x = 0;
pthread_rwlock_t rw = PTHREAD_RWLOCK_INITIALIZER;
void *worker(void *arg) { pthread_rwlock_rdlock(&rw); x++; pthread_rwlock_unlock(&rw); return 0; }
int main(void) {
  pthread_t t;
  pthread_create(&t, 0, worker, 0);
  pthread_rwlock_rdlock(&rw); x++; pthread_rwlock_unlock(&rw);   /* rd-rd 共享，都写 → race */
  pthread_join(t, 0);
  return 0;
}
```

### 运行指令

```bash
$ /home/ruan/ikos/install/bin/ikos --analyses=race --concurrency=auto -o /tmp/rep_L1_rwlock-race.db test/L1/rwlock-race.c
```

### 终端输出

```text
[*] Compiling test/L1/rwlock-race.c
[*] Running ikos preprocessor
[*] Running ikos analyzer
[*] Translating LLVM bitcode to AR
Warning: llvm function pthread_join and ar intrinsic ar.pthread.join have a different type
LLVM function declaration
ikos ar expected function declaration
Expected signature will be ignored.
[*] Running liveness analysis
[*] Running widening hint analysis
[*] Running interprocedural value analysis
[*] Race analysis requested: auto-enabling thread-modular transfer-function gates
[*] Running function pointer analysis (thread-modular)
[*] Starting global Worklist fixpoint iteration
[*] Analyzing function 'main'
[*] Analyzing function 'worker'
[*] Analyzing function 'main'
[*] Analyzing function 'worker'
[*] Analyzing function 'main'
[*] Analyzing function 'worker'
[*] Analyzing function 'main'
[*] Running post-loop checks for all analyzed functions
[*] Checking properties for function 'worker'
[*] Checking properties for function 'main'

# Time stats:
clang        : 0.052 sec
ikos-analyzer: 0.013 sec
ikos-pp      : 0.005 sec

# Summary:
Total number of checks                : 1
Total number of unreachable checks    : 0
Total number of safe checks           : 0
Total number of definite unsafe checks: 1
Total number of warnings              : 0
Total number of unknown checks        : 0

The program is definitely UNSAFE

# Results
test/L1/rwlock-race.c: In function 'main':
test/L1/rwlock-race.c:11:32: error: potential data race: conflicting memory access without a common lock
  pthread_rwlock_rdlock(&rw); x++; pthread_rwlock_unlock(&rw);   /* rd-rd 共享，都写 → race */
                               ^
```

---

## 用例 8：`L1/rwlock-safe.c`

**预期结果**: Safe　|　**实际结果**: Safe

### 源码

```c
/* 测试目标: pthread_rwlock_wrlock —— 两线程都用写锁（互斥）保护共享变量
 * 预期结果: Safe
 */
#include <pthread.h>
int x = 0;
pthread_rwlock_t rw = PTHREAD_RWLOCK_INITIALIZER;
void *worker(void *arg) { pthread_rwlock_wrlock(&rw); x++; pthread_rwlock_unlock(&rw); return 0; }
int main(void) {
  pthread_t t;
  pthread_create(&t, 0, worker, 0);
  pthread_rwlock_wrlock(&rw); x++; pthread_rwlock_unlock(&rw);   /* 写锁互斥 */
  pthread_join(t, 0);
  return 0;
}
```

### 运行指令

```bash
$ /home/ruan/ikos/install/bin/ikos --analyses=race --concurrency=auto -o /tmp/rep_L1_rwlock-safe.db test/L1/rwlock-safe.c
```

### 终端输出

```text
[*] Compiling test/L1/rwlock-safe.c
[*] Running ikos preprocessor
[*] Running ikos analyzer
[*] Translating LLVM bitcode to AR
Warning: llvm function pthread_join and ar intrinsic ar.pthread.join have a different type
LLVM function declaration
ikos ar expected function declaration
Expected signature will be ignored.
[*] Running liveness analysis
[*] Running widening hint analysis
[*] Running interprocedural value analysis
[*] Race analysis requested: auto-enabling thread-modular transfer-function gates
[*] Running function pointer analysis (thread-modular)
[*] Starting global Worklist fixpoint iteration
[*] Analyzing function 'main'
[*] Analyzing function 'worker'
[*] Analyzing function 'main'
[*] Running post-loop checks for all analyzed functions
[*] Checking properties for function 'worker'
[*] Checking properties for function 'main'

# Time stats:
clang        : 0.050 sec
ikos-analyzer: 0.012 sec
ikos-pp      : 0.005 sec

# Summary:
Total number of checks                : 0
Total number of unreachable checks    : 0
Total number of safe checks           : 0
Total number of definite unsafe checks: 0
Total number of warnings              : 0
Total number of unknown checks        : 0

The program is SAFE

# Results
No entries.
```

---

## 用例 9：`L1/spin-race.c`

**预期结果**: Race　|　**实际结果**: Race

### 源码

```c
/* 测试目标: pthread_spin_lock/unlock —— 两线程用【不同】自旋锁保护同一共享变量
 * 预期结果: Race
 */
#include <pthread.h>
int x = 0;
pthread_spinlock_t s1, s2;
void *worker(void *arg) { pthread_spin_lock(&s1); x++; pthread_spin_unlock(&s1); return 0; }
int main(void) {
  pthread_spin_init(&s1, PTHREAD_PROCESS_PRIVATE);
  pthread_spin_init(&s2, PTHREAD_PROCESS_PRIVATE);
  pthread_t t;
  pthread_create(&t, 0, worker, 0);
  pthread_spin_lock(&s2); x++; pthread_spin_unlock(&s2);   /* s2 != s1 */
  pthread_join(t, 0);
  return 0;
}
```

### 运行指令

```bash
$ /home/ruan/ikos/install/bin/ikos --analyses=race --concurrency=auto -o /tmp/rep_L1_spin-race.db test/L1/spin-race.c
```

### 终端输出

```text
[*] Compiling test/L1/spin-race.c
[*] Running ikos preprocessor
[*] Running ikos analyzer
[*] Translating LLVM bitcode to AR
Warning: llvm function pthread_join and ar intrinsic ar.pthread.join have a different type
LLVM function declaration
ikos ar expected function declaration
Expected signature will be ignored.
[*] Running liveness analysis
[*] Running widening hint analysis
[*] Running interprocedural value analysis
[*] Race analysis requested: auto-enabling thread-modular transfer-function gates
[*] Running function pointer analysis (thread-modular)
[*] Starting global Worklist fixpoint iteration
[*] Analyzing function 'main'
[*] Analyzing function 'worker'
[*] Analyzing function 'main'
[*] Analyzing function 'worker'
[*] Analyzing function 'main'
[*] Running post-loop checks for all analyzed functions
[*] Checking properties for function 'worker'
[*] Checking properties for function 'main'

# Time stats:
clang        : 0.049 sec
ikos-analyzer: 0.014 sec
ikos-pp      : 0.005 sec

# Summary:
Total number of checks                : 1
Total number of unreachable checks    : 0
Total number of safe checks           : 0
Total number of definite unsafe checks: 1
Total number of warnings              : 0
Total number of unknown checks        : 0

The program is definitely UNSAFE

# Results
test/L1/spin-race.c: In function 'main':
test/L1/spin-race.c:13:28: error: potential data race: conflicting memory access without a common lock
  pthread_spin_lock(&s2); x++; pthread_spin_unlock(&s2);   /* s2 != s1 */
                           ^
```

---

## 用例 10：`L1/spin-safe.c`

**预期结果**: Safe　|　**实际结果**: Safe

### 源码

```c
/* 测试目标: pthread_spin_lock/unlock —— 两线程用【同一把】自旋锁保护共享变量
 * 预期结果: Safe
 */
#include <pthread.h>
int x = 0;
pthread_spinlock_t s;
void *worker(void *arg) { pthread_spin_lock(&s); x++; pthread_spin_unlock(&s); return 0; }
int main(void) {
  pthread_spin_init(&s, PTHREAD_PROCESS_PRIVATE);
  pthread_t t;
  pthread_create(&t, 0, worker, 0);
  pthread_spin_lock(&s); x++; pthread_spin_unlock(&s);
  pthread_join(t, 0);
  return 0;
}
```

### 运行指令

```bash
$ /home/ruan/ikos/install/bin/ikos --analyses=race --concurrency=auto -o /tmp/rep_L1_spin-safe.db test/L1/spin-safe.c
```

### 终端输出

```text
[*] Compiling test/L1/spin-safe.c
[*] Running ikos preprocessor
[*] Running ikos analyzer
[*] Translating LLVM bitcode to AR
Warning: llvm function pthread_join and ar intrinsic ar.pthread.join have a different type
LLVM function declaration
ikos ar expected function declaration
Expected signature will be ignored.
[*] Running liveness analysis
[*] Running widening hint analysis
[*] Running interprocedural value analysis
[*] Race analysis requested: auto-enabling thread-modular transfer-function gates
[*] Running function pointer analysis (thread-modular)
[*] Starting global Worklist fixpoint iteration
[*] Analyzing function 'main'
[*] Analyzing function 'worker'
[*] Analyzing function 'main'
[*] Running post-loop checks for all analyzed functions
[*] Checking properties for function 'worker'
[*] Checking properties for function 'main'

# Time stats:
clang        : 0.050 sec
ikos-analyzer: 0.012 sec
ikos-pp      : 0.005 sec

# Summary:
Total number of checks                : 0
Total number of unreachable checks    : 0
Total number of safe checks           : 0
Total number of definite unsafe checks: 0
Total number of warnings              : 0
Total number of unknown checks        : 0

The program is SAFE

# Results
No entries.
```

---

## 用例 11：`L2/cond-mutex-race.c`

**预期结果**: Race　|　**实际结果**: Race

### 源码

```c
/* 测试目标: pthread_cond_wait 的可达性 stub —— 条件变量不保护数据，data 的无锁写读为真竞态
 * 预期结果: Race
 */
#include <pthread.h>
int data = 0;
pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t c = PTHREAD_COND_INITIALIZER;
void *worker(void *arg) { data = 1; return 0; }   /* 无锁写 data */
int main(void) {
  pthread_t t;
  pthread_create(&t, 0, worker, 0);
  pthread_mutex_lock(&m);
  pthread_cond_wait(&c, &m);         /* 无 signal，stub 仅 mem_forget_all */
  pthread_mutex_unlock(&m);
  int r = data;                       /* 与 worker 写并发 → race */
  (void)r;
  pthread_join(t, 0);
  return 0;
}
```

### 运行指令

```bash
$ /home/ruan/ikos/install/bin/ikos --analyses=race --concurrency=auto -o /tmp/rep_L2_cond-mutex-race.db test/L2/cond-mutex-race.c
```

### 终端输出

```text
[*] Compiling test/L2/cond-mutex-race.c
[*] Running ikos preprocessor
[*] Running ikos analyzer
[*] Translating LLVM bitcode to AR
Warning: llvm function pthread_join and ar intrinsic ar.pthread.join have a different type
LLVM function declaration
ikos ar expected function declaration
Expected signature will be ignored.
[*] Running liveness analysis
[*] Running widening hint analysis
[*] Running interprocedural value analysis
[*] Race analysis requested: auto-enabling thread-modular transfer-function gates
[*] Running function pointer analysis (thread-modular)
[*] Starting global Worklist fixpoint iteration
[*] Analyzing function 'main'
[*] Analyzing function 'worker'
[*] Analyzing function 'main'
[*] Analyzing function 'worker'
[*] Analyzing function 'main'
[*] Running post-loop checks for all analyzed functions
[*] Checking properties for function 'worker'
[*] Checking properties for function 'main'

# Time stats:
clang        : 0.049 sec
ikos-analyzer: 0.012 sec
ikos-pp      : 0.005 sec

# Summary:
Total number of checks                : 1
Total number of unreachable checks    : 0
Total number of safe checks           : 0
Total number of definite unsafe checks: 1
Total number of warnings              : 0
Total number of unknown checks        : 0

The program is definitely UNSAFE

# Results
test/L2/cond-mutex-race.c: In function 'worker':
test/L2/cond-mutex-race.c:8:32: error: potential data race: conflicting memory access without a common lock
void *worker(void *arg) { data = 1; return 0; }   /* 无锁写 data */
                               ^
```

---

## 用例 12：`L2/cond-nohb.c`

**预期结果**: Race　|　**实际结果**: Race

### 源码

```c
/* 测试目标: pthread_cond_wait 无 signal→wait happens-before（仅可达性 stub）——
 *           生产者写 data 后 signal，消费者 wait 后读 data，顺序仅靠条件变量，
 *           IKOS 因无 HB 误报 Race（程序本身无 race）
 * 预期结果: Race（文档化误报 FP）
 */
#include <pthread.h>
int data = 0;
int ready = 0;
pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t c = PTHREAD_COND_INITIALIZER;
void *producer(void *arg) {
  data = 42;                          /* 写 data（无锁） */
  pthread_mutex_lock(&m); ready = 1; pthread_cond_signal(&c); pthread_mutex_unlock(&m);
  return 0;
}
int main(void) {
  pthread_t t;
  pthread_create(&t, 0, producer, 0);
  pthread_mutex_lock(&m);
  while (!ready) pthread_cond_wait(&c, &m);
  pthread_mutex_unlock(&m);
  int r = data;                       /* 读 data（期望被 signal HB，IKOS 无此边） */
  (void)r;
  pthread_join(t, 0);
  return 0;
}
```

### 运行指令

```bash
$ /home/ruan/ikos/install/bin/ikos --analyses=race --concurrency=auto -o /tmp/rep_L2_cond-nohb.db test/L2/cond-nohb.c
```

### 终端输出

```text
[*] Compiling test/L2/cond-nohb.c
[*] Running ikos preprocessor
[*] Running ikos analyzer
[*] Translating LLVM bitcode to AR
Warning: llvm function pthread_join and ar intrinsic ar.pthread.join have a different type
LLVM function declaration
ikos ar expected function declaration
Expected signature will be ignored.
[*] Running liveness analysis
[*] Running widening hint analysis
[*] Running interprocedural value analysis
[*] Race analysis requested: auto-enabling thread-modular transfer-function gates
[*] Running function pointer analysis (thread-modular)
[*] Starting global Worklist fixpoint iteration
[*] Analyzing function 'main'
[*] Analyzing function 'producer'
[*] Analyzing function 'main'
[*] Analyzing function 'producer'
[*] Analyzing function 'main'
[*] Running post-loop checks for all analyzed functions
[*] Checking properties for function 'producer'
[*] Checking properties for function 'main'

# Time stats:
clang        : 0.051 sec
ikos-analyzer: 0.020 sec
ikos-pp      : 0.005 sec

# Summary:
Total number of checks                : 1
Total number of unreachable checks    : 0
Total number of safe checks           : 0
Total number of definite unsafe checks: 1
Total number of warnings              : 0
Total number of unknown checks        : 0

The program is definitely UNSAFE

# Results
test/L2/cond-nohb.c: In function 'producer':
test/L2/cond-nohb.c:12:8: error: potential data race: conflicting memory access without a common lock
  data = 42;                          /* 写 data（无锁） */
       ^
```

---

## 用例 13：`L2/pseudo-lock-distinct.c`

**预期结果**: Race　|　**实际结果**: Race

### 源码

```c
/* 测试目标: 伪锁作用域只在段内 —— 两线程用原子段保护【不同】变量 x/y，但对共享 data 的无锁访问
 *           仍应报 Race（伪锁不误保护无关数据）
 * 预期结果: Race
 */
#include <pthread.h>
extern void __VERIFIER_atomic_begin(void);
extern void __VERIFIER_atomic_end(void);
int x = 0, y = 0, data = 0;
void *t1(void *arg) { __VERIFIER_atomic_begin(); x++; __VERIFIER_atomic_end(); data++; return 0; }
void *t2(void *arg) { __VERIFIER_atomic_begin(); y++; __VERIFIER_atomic_end(); data++; return 0; }
int main(void) {
  pthread_t a, b;
  pthread_create(&a, 0, t1, 0);
  pthread_create(&b, 0, t2, 0);
  pthread_join(a, 0); pthread_join(b, 0);
  return 0;
}
```

### 运行指令

```bash
$ /home/ruan/ikos/install/bin/ikos --analyses=race --concurrency=auto -o /tmp/rep_L2_pseudo-lock-distinct.db test/L2/pseudo-lock-distinct.c
```

### 终端输出

```text
[*] Compiling test/L2/pseudo-lock-distinct.c
[*] Running ikos preprocessor
[*] Running ikos analyzer
[*] Translating LLVM bitcode to AR
Warning: llvm function pthread_join and ar intrinsic ar.pthread.join have a different type
LLVM function declaration
ikos ar expected function declaration
Expected signature will be ignored.
[*] Running liveness analysis
[*] Running widening hint analysis
[*] Running interprocedural value analysis
[*] Race analysis requested: auto-enabling thread-modular transfer-function gates
[*] Running function pointer analysis (thread-modular)
[*] Starting global Worklist fixpoint iteration
[*] Analyzing function 'main'
[*] Analyzing function 't2'
[*] Analyzing function 't1'
[*] Analyzing function 'main'
[*] Analyzing function 't2'
[*] Analyzing function 't1'
[*] Analyzing function 'main'
[*] Analyzing function 't2'
[*] Analyzing function 't1'
[*] Analyzing function 'main'
[*] Running post-loop checks for all analyzed functions
[*] Checking properties for function 't2'
[*] Checking properties for function 't1'
[*] Checking properties for function 'main'

# Time stats:
clang        : 0.049 sec
ikos-analyzer: 0.017 sec
ikos-pp      : 0.005 sec

# Summary:
Total number of checks                : 1
Total number of unreachable checks    : 0
Total number of safe checks           : 0
Total number of definite unsafe checks: 1
Total number of warnings              : 0
Total number of unknown checks        : 0

The program is definitely UNSAFE

# Results
test/L2/pseudo-lock-distinct.c: In function 't1':
test/L2/pseudo-lock-distinct.c:9:84: error: potential data race: conflicting memory access without a common lock
void *t1(void *arg) { __VERIFIER_atomic_begin(); x++; __VERIFIER_atomic_end(); data++; return 0; }
                                                                                   ^
```

---

## 用例 14：`L2/pseudo-lock-shared.c`

**预期结果**: Safe　|　**实际结果**: Safe

### 源码

```c
/* 测试目标: __VERIFIER_atomic_begin/end 单一共享伪锁（0xA7E0AD1C）—— 同一共享变量在两段内自增，
 *           伪锁提供互斥
 * 预期结果: Safe
 */
#include <pthread.h>
extern void __VERIFIER_atomic_begin(void);
extern void __VERIFIER_atomic_end(void);
int x = 0;
void *worker(void *arg) { __VERIFIER_atomic_begin(); x++; __VERIFIER_atomic_end(); return 0; }
int main(void) {
  pthread_t t;
  pthread_create(&t, 0, worker, 0);
  __VERIFIER_atomic_begin(); x++; __VERIFIER_atomic_end();
  pthread_join(t, 0);
  return 0;
}
```

### 运行指令

```bash
$ /home/ruan/ikos/install/bin/ikos --analyses=race --concurrency=auto -o /tmp/rep_L2_pseudo-lock-shared.db test/L2/pseudo-lock-shared.c
```

### 终端输出

```text
[*] Compiling test/L2/pseudo-lock-shared.c
[*] Running ikos preprocessor
[*] Running ikos analyzer
[*] Translating LLVM bitcode to AR
Warning: llvm function pthread_join and ar intrinsic ar.pthread.join have a different type
LLVM function declaration
ikos ar expected function declaration
Expected signature will be ignored.
[*] Running liveness analysis
[*] Running widening hint analysis
[*] Running interprocedural value analysis
[*] Race analysis requested: auto-enabling thread-modular transfer-function gates
[*] Running function pointer analysis (thread-modular)
[*] Starting global Worklist fixpoint iteration
[*] Analyzing function 'main'
[*] Analyzing function 'worker'
[*] Analyzing function 'main'
[*] Running post-loop checks for all analyzed functions
[*] Checking properties for function 'worker'
[*] Checking properties for function 'main'

# Time stats:
clang        : 0.052 sec
ikos-analyzer: 0.010 sec
ikos-pp      : 0.004 sec

# Summary:
Total number of checks                : 0
Total number of unreachable checks    : 0
Total number of safe checks           : 0
Total number of definite unsafe checks: 0
Total number of warnings              : 0
Total number of unknown checks        : 0

The program is SAFE

# Results
No entries.
```

---

## 用例 15：`L2/unknown-mixed-atomic.c`

**预期结果**: Unknown　|　**实际结果**: Unknown

### 源码

```c
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
```

### 运行指令

```bash
$ /home/ruan/ikos/install/bin/ikos --analyses=race --concurrency=auto -o /tmp/rep_L2_unknown-mixed-atomic.db test/L2/unknown-mixed-atomic.c
```

### 终端输出

```text
[*] Compiling test/L2/unknown-mixed-atomic.c
[*] Running ikos preprocessor
[*] Running ikos analyzer
[*] Translating LLVM bitcode to AR
Warning: llvm function pthread_join and ar intrinsic ar.pthread.join have a different type
LLVM function declaration
ikos ar expected function declaration
Expected signature will be ignored.
[*] Running liveness analysis
[*] Running widening hint analysis
[*] Running interprocedural value analysis
[*] Race analysis requested: auto-enabling thread-modular transfer-function gates
[*] Running function pointer analysis (thread-modular)
[*] Starting global Worklist fixpoint iteration
[*] Analyzing function 'main'
[*] Analyzing function 't'
[*] Analyzing function 'u'
[*] Analyzing function 'main'
[*] Running post-loop checks for all analyzed functions
[*] Checking properties for function 't'
[*] Checking properties for function 'u'
[*] Checking properties for function 'main'

# Time stats:
clang        : 0.052 sec
ikos-analyzer: 0.011 sec
ikos-pp      : 0.004 sec

# Summary:
Total number of checks                : 1
Total number of unreachable checks    : 0
Total number of safe checks           : 0
Total number of definite unsafe checks: 0
Total number of warnings              : 0
Total number of unknown checks        : 1

The program is UNKNOWN

# Results
test/L2/unknown-mixed-atomic.c: In function 't':
test/L2/unknown-mixed-atomic.c:9:22: warning: potential data race: conflicting memory access without a common lock
void *t(void *arg) { atomic_store(&glob, 1); return 0; }   /* 原子写 */
                     ^
```

---

## 用例 16：`L2/unknown-pointer-alias.c`

**预期结果**: Race　|　**实际结果**: Race

### 源码

```c
/* 测试目标: Unknown 边界反例 —— 纯指针别名模糊（多态锁）致锁被丢弃，数据访问 points-to 具体，
 *           应报 Race 而非 Unknown
 * 预期结果: Race
 */
#include <pthread.h>
int x = 0;
pthread_mutex_t m1 = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t m2 = PTHREAD_MUTEX_INITIALIZER;
extern int __VERIFIER_nondet_int(void);
void *worker(void *arg) {
  pthread_mutex_t *mp = __VERIFIER_nondet_int() ? &m1 : &m2;   /* 多态锁 {m1,m2} */
  pthread_mutex_lock(mp); x++; pthread_mutex_unlock(mp);
  return 0;
}
int main(void) {
  pthread_t t;
  pthread_create(&t, 0, worker, 0);
  pthread_mutex_t *mp = __VERIFIER_nondet_int() ? &m1 : &m2;
  pthread_mutex_lock(mp); x++; pthread_mutex_unlock(mp);
  pthread_join(t, 0);
  return 0;
}
```

### 运行指令

```bash
$ /home/ruan/ikos/install/bin/ikos --analyses=race --concurrency=auto -o /tmp/rep_L2_unknown-pointer-alias.db test/L2/unknown-pointer-alias.c
```

### 终端输出

```text
[*] Compiling test/L2/unknown-pointer-alias.c
[*] Running ikos preprocessor
[*] Running ikos analyzer
[*] Translating LLVM bitcode to AR
Warning: llvm function pthread_join and ar intrinsic ar.pthread.join have a different type
LLVM function declaration
ikos ar expected function declaration
Expected signature will be ignored.
[*] Running liveness analysis
[*] Running widening hint analysis
[*] Running interprocedural value analysis
[*] Race analysis requested: auto-enabling thread-modular transfer-function gates
[*] Running function pointer analysis (thread-modular)
[*] Starting global Worklist fixpoint iteration
[*] Analyzing function 'main'
[*] Analyzing function 'worker'
[*] Analyzing function 'main'
[*] Analyzing function 'worker'
[*] Analyzing function 'main'
[*] Analyzing function 'worker'
[*] Analyzing function 'main'
[*] Running post-loop checks for all analyzed functions
[*] Checking properties for function 'worker'
[*] Checking properties for function 'main'

# Time stats:
clang        : 0.050 sec
ikos-analyzer: 0.016 sec
ikos-pp      : 0.005 sec

# Summary:
Total number of checks                : 1
Total number of unreachable checks    : 0
Total number of safe checks           : 0
Total number of definite unsafe checks: 1
Total number of warnings              : 0
Total number of unknown checks        : 0

The program is definitely UNSAFE

# Results
test/L2/unknown-pointer-alias.c: In function 'main':
test/L2/unknown-pointer-alias.c:19:28: error: potential data race: conflicting memory access without a common lock
  pthread_mutex_lock(mp); x++; pthread_mutex_unlock(mp);
                           ^
```

---

