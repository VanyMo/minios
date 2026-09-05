// 磁盘块缓存（Buffer cache）。
//
// 缓存里放着一组 struct buf，保存磁盘块内容的内存副本。
// 缓存磁盘块可以减少读磁盘的次数，同时也为多个进程共用
// 同一个磁盘块提供了同步点（同一个块同一时刻只有一个进程能用）。
//
// 接口（这部分与原来完全一致）：
// * 想读某个磁盘块：调用 bread。
// * 修改完缓冲区数据后：调用 bwrite 写回磁盘。
// * 用完缓冲区后：调用 brelse 释放。
// * 调用 brelse 之后不要再使用这个缓冲区。
// * 一次只能有一个进程使用一个缓冲区，所以别拿太久。
//
// 【本实验的改动（Lab: Lock - Buffer cache）】
// 原来的实现用一把全局锁 bcache.lock 保护所有 30 个缓冲区，
// 所有 CPU 的每次查找/释放都要抢同一把锁，竞争非常严重。
//
// 改进方案：把“一条全局链表”改造成“哈希表 + 每个桶一把锁”：
// - 用块号计算哈希值，把 30 个缓冲区分散挂在 13 个桶（bucket）里，
//   每个桶是一条双向链表，有自己独立的自旋锁。
// - 查找命中（最常见路径）：只需要拿“目标块所在那一个桶”的锁。
//   不同进程访问落在不同桶里的块时完全并行，互不冲突。
// - 释放 brelse / bpin / bunpin：也只拿该缓冲区所在桶的锁。
// - 缓存未命中需要“淘汰一个空闲缓冲区”时：这一步允许串行化，
//   由一把全局 eviction 锁保护（这种情况少，串行也没关系）。
//
// 所有锁的名字都以 "bcache" 开头：
//   全局淘汰锁叫 "bcache"，桶锁叫 "bcache.bucket"。
//
// 维持的关键不变式（invariant）：
// 1. 每个磁盘块在缓存中最多只有一份副本（一个 buf 只属于一个桶）。
// 2. buf 的身份（dev/blockno）只能在其 refcnt==0 且持有
//    全局淘汰锁 + 相关桶锁时才能被修改。
// 3. refcnt 的增减必须持有该 buf 所在桶的桶锁。

#include "types.h"      // 基本类型定义，如 uint/uchar
#include "param.h"      // 包含 NBUF（缓冲区个数 = 30）、NCPU 等常量
#include "spinlock.h"   // 自旋锁 struct spinlock 与 acquire/release 等
#include "sleeplock.h"  // 睡眠锁 struct sleeplock 与 acquiresleep 等
#include "riscv.h"      // RISC-V 相关定义
#include "defs.h"       // 内核函数声明汇总
#include "fs.h"         // 文件系统相关定义（含 BSIZE 块大小 1024）
#include "buf.h"        // struct buf 缓冲区结构定义

// 哈希桶的个数。选一个质数（13）能让块号分布得更均匀，
// 减少两个进程的块恰好落在同一个桶里互相等锁的概率。
#define NBUCKET 13

struct {
  // 【改动】全局“淘汰锁”：只在缓存未命中、需要挑一个空闲缓冲区
  // 复用（eviction，淘汰）时才获取。它把并发的“未命中”处理串行化，
  // 保证同一时刻只有一个 CPU 在改缓冲区的身份（dev/blockno）。
  struct spinlock eviction_lock;

  // 【改动】每个哈希桶一把锁，保护该桶的链表以及链上 buf 的 refcnt。
  struct spinlock bucket_lock[NBUCKET];

  // 缓冲区池：仍然是固定 NBUF(30) 个，不许增加（实验要求）。
  struct buf buf[NBUF];

  // 【改动】13 个桶，每个桶是一个双向链表，bucket[i] 是假头节点
  // （dummy head），bucket[i].next 是链上第一个真正的缓冲区。
  // 原来单一的全局 LRU 链表（bcache.head）被删掉了 —— 实验允许不做 LRU。
  struct buf bucket[NBUCKET];
} bcache;

// 【新增】哈希函数：根据设备号和块号算出该块应该放在哪个桶。
// 取模 NBUCKET(13) 把块号打散到 13 个桶里。
static int
hashbucket(uint dev, uint blockno)
{
  // 把设备号和块号混在一起再取模（dev 乘一个质数 131 仅为让它也影响结果；
  // 实际上 xv6 里 dev 几乎总是 ROOTDEV=1，起主要作用的是 blockno）
  return (dev * 131 + blockno) % NBUCKET;
}

// 【改动】初始化缓冲区缓存：初始化所有锁和 13 个桶的空链表。
void
binit(void)
{
  struct buf *b;

  // 初始化全局淘汰锁，名字必须以 "bcache" 开头
  initlock(&bcache.eviction_lock, "bcache");

  // 初始化 13 个桶：每把桶锁起名 "bcache.bucket"（同样以 "bcache" 开头），
  // 并把每个桶的链表置空（假头节点的 prev/next 都指向自己 = 空链表）。
  for(int i = 0; i < NBUCKET; i++){
    initlock(&bcache.bucket_lock[i], "bcache.bucket");
    bcache.bucket[i].prev = &bcache.bucket[i];  // 空链表：头的前驱指向自己
    bcache.bucket[i].next = &bcache.bucket[i];  // 空链表：头的后继指向自己
  }

  // 初始化 30 个缓冲区：每个配一把睡眠锁（供进程独占使用缓冲区时睡眠等待），
  // 并把所有缓冲区都挂到 0 号桶的链表上（刚开机时还没有任何缓存内容，
  // 全放一个桶没关系，第一次用到时会被搬到正确的桶）。
  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    b->next = bcache.bucket[0].next;     // 新缓冲区指向 0 号桶原来的第一个节点
    b->prev = &bcache.bucket[0];         // 新缓冲区的前驱是 0 号桶假头
    initsleeplock(&b->lock, "buffer");   // 初始化这个 buf 的睡眠锁
    bcache.bucket[0].next->prev = b;     // 原第一个节点的 prev 指向新节点
    bcache.bucket[0].next = b;           // 假头的 next 指向新节点（头插完成）
  }
}

// 在缓存中查找设备 dev 上的 blockno 块。
// 找不到就淘汰一个空闲缓冲区来装载这个块。
// 无论哪种情况，返回时都持有该缓冲区的睡眠锁（已上锁）。
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;

  // 先算出目标块属于哪个桶，之后所有操作都围绕这个桶进行
  int idx = hashbucket(dev, blockno);

  // ---------- 第一阶段：快速查找（只需要一把桶锁）----------
  // 拿住目标桶的锁。注意：其他 CPU 如果访问的是落在别的桶里的块，
  // 拿的是别的桶锁，双方完全并行 —— 这就是消除竞争的关键路径。
  acquire(&bcache.bucket_lock[idx]);

  // 遍历桶 idx 的双向链表（从假头的 next 开始，走回假头为止）
  for(b = bcache.bucket[idx].next; b != &bcache.bucket[idx]; b = b->next){
    // 如果设备号和块号都匹配，说明这个块已经在缓存里了（命中）
    if(b->dev == dev && b->blockno == blockno){
      // 引用计数 +1，表示“我正在使用这个缓冲区”，这样它就不会被淘汰。
      // refcnt 的修改必须在桶锁保护下进行（见文件头的不变式 3）。
      b->refcnt++;
      release(&bcache.bucket_lock[idx]);  // 尽早放锁，缩短临界区
      acquiresleep(&b->lock);             // 等着独占这个缓冲区（可能睡眠）
      return b;                            // 返回持有睡眠锁的缓冲区
    }
  }

  // 走到这里说明在桶里没找到 —— 缓存未命中（miss）。
  // 先放掉桶锁，因为接下来要拿全局淘汰锁；
  // 锁的顺序必须是“先淘汰锁、后桶锁”，反过来拿就可能死锁。
  release(&bcache.bucket_lock[idx]);

  // ---------- 第二阶段：未命中，挑一个空闲缓冲区来复用 ----------
  // 全局淘汰锁把所有“未命中”的处理串行化：
  // 同一时刻只有一个 CPU 能挑选/修改空闲缓冲区的身份。
  acquire(&bcache.eviction_lock);

  // 重要：拿不到桶锁的这段时间里，别的 CPU 可能刚好把同一个块
  // 装进缓存了。所以必须重新查一遍桶（“double-check”），
  // 否则同一个块会被装进两个缓冲区，违反“每个块最多一份副本”。
  acquire(&bcache.bucket_lock[idx]);
  for(b = bcache.bucket[idx].next; b != &bcache.bucket[idx]; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      // 别人已经装好了，直接复用：引用计数 +1 后按命中路径返回
      b->refcnt++;
      release(&bcache.bucket_lock[idx]);   // 放锁顺序与加锁顺序相反
      release(&bcache.eviction_lock);
      acquiresleep(&b->lock);
      return b;
    }
  }
  // 第二次确认也没找到，才真正需要淘汰一个缓冲区。

  // 在所有桶里找一个 refcnt==0（没人用）的缓冲区。
  // 原来是找“最近最少使用(LRU)”的那个；实验说明允许退化成
  // “任意一个空闲的都行”，这样 brelse 就不用维护 LRU 链表、
  // 也就不需要在释放路径上碰任何全局结构。
  for(int i = 0; i < NBUCKET; i++){
    // 桶 idx 的锁我们已经拿着了（上面 acquire 过），不要重复获取，
    // 重复获取同一把锁会 panic("acquire")
    if(i == idx)
      continue;  // idx 桶稍后在循环外统一检查（见下方 i == idx 的处理）

    // 拿住第 i 个桶的锁才能看它的链表和 refcnt。
    // 死锁分析：此时我们持有 淘汰锁 + 桶idx + 桶i。
    // 别的路径（命中/brelse/bpin）一次只拿一把桶锁，绝不会拿第二把；
    // 想同时拿多把桶锁的只有“持淘汰锁的未命中路径”，
    // 而淘汰锁保证这条路径全局只有一个 —— 所以不会死锁。
    acquire(&bcache.bucket_lock[i]);

    for(b = bcache.bucket[i].next; b != &bcache.bucket[i]; b = b->next){
      if(b->refcnt == 0){
        // 找到空闲缓冲区！先把它从旧桶 i 的链表上摘下来：
        b->next->prev = b->prev;   // 前后节点互相牵手，跳过 b
        b->prev->next = b->next;

        // 再头插到目标桶 idx 的链表上（因为新块号哈希到 idx）：
        b->next = bcache.bucket[idx].next;
        b->prev = &bcache.bucket[idx];
        bcache.bucket[idx].next->prev = b;
        bcache.bucket[idx].next = b;

        // 更新缓冲区身份：从此它代表 (dev, blockno) 这一块。
        // 因为 refcnt 原来是 0（没人用），现在改成 1（我在用），
        // 身份变更全程持有淘汰锁 + 两把桶锁，是安全的。
        b->dev = dev;
        b->blockno = blockno;
        b->valid = 0;   // 数据还没从磁盘读进来，标记为无效
        b->refcnt = 1;  // 我引用它一次

        // 按相反顺序放掉三把锁，然后等睡眠锁，返回
        release(&bcache.bucket_lock[i]);
        release(&bcache.bucket_lock[idx]);
        release(&bcache.eviction_lock);
        acquiresleep(&b->lock);
        return b;
      }
    }
    release(&bcache.bucket_lock[i]);  // 桶 i 里没有空闲的，放锁试下一个桶
  }

  // 还有一种情况没查：目标桶 idx 本身（循环里被 continue 跳过了）。
  // 此刻我们仍然持有 idx 的锁，单独扫一遍它。
  for(b = bcache.bucket[idx].next; b != &bcache.bucket[idx]; b = b->next){
    if(b->refcnt == 0){
      // 情况特殊一点：新块和旧块哈希到同一个桶（就是 idx），
      // 所以不需要搬动链表节点 —— 它已经在正确的桶里了，
      // 只需要更新身份字段。这时我们其实只持有 idx 一把桶锁，
      // 不会有任何死锁问题（hint 里专门提醒过这种同桶情况）。
      b->dev = dev;
      b->blockno = blockno;
      b->valid = 0;    // 数据尚未从磁盘读取
      b->refcnt = 1;   // 我引用它一次

      release(&bcache.bucket_lock[idx]);
      release(&bcache.eviction_lock);
      acquiresleep(&b->lock);
      return b;
    }
  }

  release(&bcache.bucket_lock[idx]);
  release(&bcache.eviction_lock);
  // 30 个缓冲区全都在被使用（refcnt>0），系统出问题了
  panic("bget: no buffers");
}

// 读取指定块：返回一个已上锁（睡眠锁）且装好了该块内容的缓冲区。
// 本函数没有改动。
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);      // 先在缓存里找（或淘汰装载）
  if(!b->valid) {              // 如果数据还没从磁盘读进来（bget 刚装的新块）
    virtio_disk_rw(b, 0);      // 从磁盘把块内容读到 b->data（0 表示读）
    b->valid = 1;              // 现在数据有效了
  }
  return b;
}

// 把 b 的内容写回磁盘。调用者必须持有 b 的睡眠锁。没有改动。
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))  // 没持锁就写？这是编程错误
    panic("bwrite");
  virtio_disk_rw(b, 1);        // 1 表示写盘
}

// 释放一个使用完的缓冲区（调用者必须先持有它的睡眠锁）。
// 【改动】原来要在全局 bcache.lock 下更新 LRU 链表；
// 现在没有 LRU 链表了，只需要在“该 buf 所在桶”的锁下把 refcnt 减 1。
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))  // 没持锁就释放？这是编程错误
    panic("brelse");

  releasesleep(&b->lock);      // 先放掉睡眠锁，允许别人使用这个缓冲区

  // 注意：此时 b 的 dev/blockno 仍然有效、可以用来算桶号 ——
  // 因为只有 refcnt==0 的缓冲区才会被改身份（淘汰），
  // 而我们的那次引用（refcnt≥1）此刻还没减掉，所以没人能动它。
  int i = hashbucket(b->dev, b->blockno);

  acquire(&bcache.bucket_lock[i]);  // 只拿这一把桶锁
  b->refcnt--;                      // 引用计数减 1；减到 0 就可以被淘汰了
  release(&bcache.bucket_lock[i]);
}

// 固定住缓冲区（引用计数 +1，常用于日志模块防止它被淘汰）。
// 【改动】原来用全局 bcache.lock，现在改用该 buf 所在桶的锁。
// 调用者持有 b 的睡眠锁 ⇒ refcnt≥1 ⇒ 身份不会被改 ⇒ 算出的桶号是稳定的。
void
bpin(struct buf *b) {
  int i = hashbucket(b->dev, b->blockno);
  acquire(&bcache.bucket_lock[i]);  // 拿该 buf 所在桶的锁
  b->refcnt++;                      // 引用计数 +1
  release(&bcache.bucket_lock[i]);
}

// 解除固定（与 bpin 配对，引用计数 -1）。同样改用桶锁。
void
bunpin(struct buf *b) {
  int i = hashbucket(b->dev, b->blockno);
  acquire(&bcache.bucket_lock[i]);  // 拿该 buf 所在桶的锁
  b->refcnt--;                      // 引用计数 -1
  release(&bcache.bucket_lock[i]);
}
