// =====================================================================
// notxv6/ph.c —— 多线程哈希表（带锁）
// =====================================================================
//
// 本实验用 POSIX pthread 线程库实现一个并发哈希表，并和 xv6 没有关系，
// 直接在宿主 Linux/macOS 上编译运行（make ph）。
//
// 单线程版本是正确的——同一时刻只有一个 put 在操作 table[]。
// 多线程时多个 put 同时跑，会有“丢失插入”的 race：
//   线程 A: i = key % NBUCKET; e = table[i]; // 假设为 NULL
//   线程 B: i = key % NBUCKET; e = table[i]; // 同样为 NULL
//   A: insert(... &table[i], table[i]);      // 把 entry 挂上
//   B: insert(... &table[i], table[i]);      // 覆盖 A 的 entry！
// 后果：A 刚插进去的 key 在 table 里看不见了。
//
// 解法：每个 hash bucket 一把锁 (mutex)。put/get 之前只锁自己
// 那个 bucket 即可，不影响其他 bucket 的并发。

#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <assert.h>
#include <pthread.h>
#include <sys/time.h>

// NBUCKET 哈希桶数；NKEYS 总 key 数
#define NBUCKET 5
#define NKEYS 100000

// 一个 hash 链表节点
struct entry {
  int key;            // 哈希的 key
  int value;          // 对应的 value
  struct entry *next; // 下一个 entry
};
struct entry *table[NBUCKET];

// 全局共享的 keys 数组，多线程只读不写，无需保护
int keys[NKEYS];
// 线程数（命令行参数指定）
int nthread = 1;

// ------- 关键：每个 bucket 一把锁 -------
// locks[i] 专门保护 table[i] 这条链表。不同 bucket 上的 put 可以
// 并发执行，从而实现 ph_fast 要求的“1.25x” 速度。
pthread_mutex_t locks[NBUCKET];


double
now()
{
 struct timeval tv;
 gettimeofday(&tv, 0);
 return tv.tv_sec + tv.tv_usec / 1000000.0;
}

// 在链表头部插入一个 entry。p 是指向 &table[i] 或上一个 entry->next
// 的指针，n 是当前 *p 指向的第一个节点。
static void
insert(int key, int value, struct entry **p, struct entry *n)
{
  struct entry *e = malloc(sizeof(struct entry));
  e->key = key;
  e->value = value;
  e->next = n;   // 让新节点指向原链表头
  *p = e;        // 把上一个指针指向新节点
}

// put：把 (key, value) 放进哈希表。
// 必须保证：put 在持有 locks[i] 时执行 (i = key % NBUCKET)，
// 这样同一 bucket 上的 put 串行化，不同 bucket 的 put 可以并行。
static
void put(int key, int value)
{
  int i = key % NBUCKET;

  // is the key already present?
  struct entry *e = 0;
  for (e = table[i]; e != 0; e = e->next) {
    if (e->key == key)
      break;
  }
  if(e){
    // update the existing key.
    e->value = value;
  } else {
    // the new is new.
    insert(key, value, &table[i], table[i]);
  }
}

// get：查找 key。read-only，仍需在持有锁时执行以避免读到一半
// 正在被插入的中间状态（保证内存可见性）。
static struct entry*
get(int key)
{
  int i = key % NBUCKET;


  struct entry *e = 0;
  for (e = table[i]; e != 0; e = e->next) {
    if (e->key == key) break;
  }

  return e;
}

// ---------------- put 的“安全+快”版本 ----------------
// 加锁放在外层（只锁一次），避免在循环里反复加解锁造成额外开销。
// 这是 ph_safe + ph_fast 都需要的版本。
static void
put_safe(int key, int value)
{
  int i = key % NBUCKET;

  pthread_mutex_lock(&locks[i]);   // 进入临界区：独占 bucket i

  struct entry *e = 0;
  for (e = table[i]; e != 0; e = e->next) {
    if (e->key == key)
      break;
  }
  if(e){
    e->value = value;
  } else {
    insert(key, value, &table[i], table[i]);
  }

  pthread_mutex_unlock(&locks[i]); // 离开临界区
}

static struct entry *
get_safe(int key)
{
  int i = key % NBUCKET;

  pthread_mutex_lock(&locks[i]);

  struct entry *e = 0;
  for (e = table[i]; e != 0; e = e->next) {
    if (e->key == key) break;
  }

  pthread_mutex_unlock(&locks[i]);
  return e;
}

// put_thread/get_thread 与原版完全一样，只是把 put/get 换成安全版本。
static void *
put_thread(void *xa)
{
  int n = (int) (long) xa; // thread number
  int b = NKEYS/nthread;

  for (int i = 0; i < b; i++) {
    put_safe(keys[b*n + i], n);
  }

  return NULL;
}

static void *
get_thread(void *xa)
{
  int n = (int) (long) xa; // thread number
  int missing = 0;

  for (int i = 0; i < NKEYS; i++) {
    struct entry *e = get_safe(keys[i]);
    if (e == 0) missing++;
  }
  printf("%d: %d keys missing\n", n, missing);
  return NULL;
}

int
main(int argc, char *argv[])
{
  pthread_t *tha;
  void *value;
  double t1, t0;


  if (argc < 2) {
    fprintf(stderr, "Usage: %s nthreads\n", argv[0]);
    exit(-1);
  }
  nthread = atoi(argv[1]);
  tha = malloc(sizeof(pthread_t) * nthread);
  srandom(0);
  assert(NKEYS % nthread == 0);
  for (int i = 0; i < NKEYS; i++) {
    keys[i] = random();
  }

  // 初始化 NBUCKET 把互斥锁
  for (int i = 0; i < NBUCKET; i++) {
    assert(pthread_mutex_init(&locks[i], NULL) == 0);
  }

  //
  // first the puts
  //
  t0 = now();
  for(int i = 0; i < nthread; i++) {
    assert(pthread_create(&tha[i], NULL, put_thread, (void *) (long) i) == 0);
  }
  for(int i = 0; i < nthread; i++) {
    assert(pthread_join(tha[i], &value) == 0);
  }
  t1 = now();

  printf("%d puts, %.3f seconds, %.0f puts/second\n",
         NKEYS, t1 - t0, NKEYS / (t1 - t0));

  //
  // now the gets
  //
  t0 = now();
  for(int i = 0; i < nthread; i++) {
    assert(pthread_create(&tha[i], NULL, get_thread, (void *) (long) i) == 0);
  }
  for(int i = 0; i < nthread; i++) {
    assert(pthread_join(tha[i], &value) == 0);
  }
  t1 = now();

  printf("%d gets, %.3f seconds, %.0f gets/second\n",
         NKEYS*nthread, t1 - t0, (NKEYS*nthread) / (t1 - t0));
}
