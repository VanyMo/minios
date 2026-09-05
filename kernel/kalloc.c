// 物理内存分配器（Physical memory allocator）：
// 为用户进程、内核栈、页表页、管道缓冲区分配整页（4096 字节）内存。
//
// 【本实验的改动（Lab: Lock - Memory allocator）】
// 原来的实现只有一个全局空闲链表 + 一把全局锁 kmem.lock，
// 多个 CPU 同时 kalloc/kfree 时都要抢这一把锁，竞争（contention）很严重。
//
// 改进方案：每个 CPU 维护一条自己的空闲链表，每条链表配一把自己的锁。
// - kfree()：把页放回“当前 CPU”的链表（只拿当前 CPU 的锁）。
// - kalloc()：优先从“当前 CPU”的链表取页（只拿当前 CPU 的锁）。
// - 偷页（steal）：如果当前 CPU 的链表是空的，就遍历其他 CPU 的链表，
//   从中“偷”一页过来用（此时才需要拿别的 CPU 的锁，这种情况很少发生）。
// 这样不同 CPU 的分配/释放就可以真正并行，锁竞争接近于 0。
//
// 所有锁的名字都以 "kmem" 开头（本实现统一叫 "kmem"），
// 这样 kalloctest 的 statistics 系统调用才能统计到它们。

#include "types.h"      // 基本类型定义，如 uint64
#include "param.h"      // 包含 NCPU（最大 CPU 数量）等常量
#include "memlayout.h"  // 内存布局常量，如 PHYSTOP（物理内存顶端）
#include "spinlock.h"   // 自旋锁结构 struct spinlock 及函数声明
#include "riscv.h"      // RISC-V 特权寄存器读取函数（cpuid 会用到 tp 寄存器）
#include "defs.h"       // 内核函数声明汇总（kalloc/kfree/push_off 等）

void freerange(void *pa_start, void *pa_end);

extern char end[]; // 内核结束后面的第一个地址（由链接脚本 kernel.ld 定义），
                   // 内核之上的物理内存从这里开始可以自由分配。

// 空闲链表的节点：就放在被释放的物理页自己的开头。
// 因为这页内存是空闲的，可以先把 next 指针写在页的开头 8 个字节里，
// 链表本身不需要额外占用内存。
struct run {
  struct run *next;  // 指向链表中下一个空闲页；0 (NULL) 表示链表结束
};

// 【改动】原来是单个 kmem 结构（一把锁 + 一条链表），
// 现在改成数组：每个 CPU 拥有一个这样的结构。
// kmems[0] 给 CPU 0 用，kmems[1] 给 CPU 1 用，以此类推（NCPU=8）。
struct kmem {
  struct spinlock lock;   // 保护本 CPU 空闲链表的自旋锁
  struct run *freelist;   // 本 CPU 的空闲页链表头指针；0 表示链表为空
};
struct kmem kmems[NCPU];  // 每个 CPU 一份分配器状态

// 【改动】初始化内存分配器：为每个 CPU 的锁分别调用 initlock。
void
kinit()
{
  // 遍历所有 CPU（NCPU 是 param.h 里定义的最大 CPU 数，本实验为 8）
  for(int i = 0; i < NCPU; i++){
    // 给第 i 个 CPU 的锁起名字；名字必须以 "kmem" 开头，
    // kalloctest 打印统计时靠名字前缀来找这些锁。
    // (即使所有锁同名 "kmem" 也没问题，hint 里明确说了可以)
    initlock(&kmems[i].lock, "kmem");
  }
  // 把 [end, PHYSTOP) 之间的所有物理页释放进空闲链表。
  // freerange 会调用 kfree，而 kfree 把页放进“正在运行 kinit 的那个 CPU”
  // （也就是 CPU 0，因为 kinit 在 main 里 hart 0 上被调用）的链表里。
  // 这符合 hint："Let freerange give all free memory to the CPU running freerange."
  freerange(end, (void*)PHYSTOP);
}

// 把 [pa_start, pa_end) 范围内的物理内存按页对齐后逐页释放。
// 本函数没有改动：它只是循环调用 kfree。
void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  // PGROUNDUP 把起始地址向上取整到 4096 的倍数（页边界），
  // 避免释放一个不完整、未对齐的“页”。
  p = (char*)PGROUNDUP((uint64)pa_start);
  // 只要还装得下一整页，就释放这一页，然后 p 前进一页（4096 字节）
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// 释放 pa 指向的一页物理内存。
// 通常 pa 应该是之前 kalloc() 返回的地址；
// 例外情况是初始化分配器时（见上面的 kinit）。
// 【改动】现在把页放回“当前 CPU”的空闲链表，而不是全局链表。
void
kfree(void *pa)
{
  struct run *r;

  // 合法性检查：地址必须按页对齐、在内核结束之后、低于 PHYSTOP，
  // 否则说明有人释放了非法地址，直接 panic 停机报告错误。
  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // 把整页填成垃圾值 1：如果有人释放后还在偷偷用这页内存（悬空引用），
  // 读到的会是垃圾，更容易暴露 bug。
  memset(pa, 1, PGSIZE);

  // 把这页内存的开头当作 struct run 使用（复用空闲内存自身存链表指针）
  r = (struct run*)pa;

  // cpuid() 返回当前是哪个 CPU 在运行，但它只有在“关中断”时才能安全使用：
  // 如果开着中断，定时器中断可能把我们调度到另一个 CPU 上，
  // 那时读到的 CPU 编号就过期了。所以先用 push_off() 关中断拿到编号，
  // 一直到链表操作结束才 pop_off() 恢复，保证 id 全程有效。
  push_off();
  int id = cpuid();      // 记住当前 CPU 的编号（0 ~ NCPU-1）

  // 只获取“当前 CPU”链表的锁 —— 其他 CPU 同时释放它们自己的页时
  // 拿的是别的锁，互不干扰，这就是消除竞争的关键。
  acquire(&kmems[id].lock);
  // 经典链表“头插法”：新释放的页指向原来的链表头……
  r->next = kmems[id].freelist;
  // ……再让链表头指向这个新页。这样这页就插到了链表最前面。
  kmems[id].freelist = r;
  release(&kmems[id].lock);
  // 恢复中断（与上面的 push_off 配对）
  pop_off();
}

// 分配一页 4096 字节的物理内存。
// 返回内核可用的指针；如果分配失败返回 0。
// 【改动】优先用当前 CPU 的链表；为空时从其他 CPU 的链表“偷”一页。
void *
kalloc(void)
{
  struct run *r = 0;  // r 用来保存取到的空闲页；先初始化为 0（空）

  // 整个 kalloc 期间保持关中断（push_off/pop_off 配对）：
  // 因为我们先在 CPU id 的链表里找，找不到又要去别的 CPU 链表偷，
  // 中途如果被中断调度到别的 CPU，之前记下的 id 就作废了，
  // 可能重复跳过/漏看某些链表。保持关中断能保证 id 一直有效。
  push_off();
  int id = cpuid();  // 当前 CPU 编号

  // 第一步：尝试从自己 CPU 的链表里取一页（最常见、无竞争的快路径）
  acquire(&kmems[id].lock);
  r = kmems[id].freelist;       // r 指向链表的第一个空闲页（可能为 0 表示空）
  if(r)                          // 如果链表不空
    kmems[id].freelist = r->next; // 链表头后移一个节点，这页就被“取走”了
  release(&kmems[id].lock);

  // 第二步：如果自己的链表是空的，就去别的 CPU 的链表里“偷”一页。
  // 注意：acquire/release 自身也会 push_off/pop_off，这里是嵌套的，
  // xv6 的 noff 计数器可以正确处理多层嵌套。
  if(!r){
    // 依次尝试其他所有 CPU 的链表
    for(int c = 0; c < NCPU; c++){
      if(c == id)            // 跳过自己：自己的链表已经确认是空的了
        continue;
      // 拿住目标 CPU c 的锁才能动它的链表
      acquire(&kmems[c].lock);
      // 如果目标 CPU 的链表里有空闲页，就从它链表头偷走一页。
      // （更复杂的方案可以一次偷半条链表，但那需要同时持有两把锁，
      //  两个 CPU 互偷时可能死锁；每次只偷一页最简单也最安全。）
      if(kmems[c].freelist){
        r = kmems[c].freelist;           // r 指向受害者的第一个空闲页
        kmems[c].freelist = r->next;     // 受害者的链表头后移，这页归我们了
        release(&kmems[c].lock);         // 立刻放掉受害者的锁，减小持锁时间
        break;                           // 偷到了，跳出循环
      }
      release(&kmems[c].lock);           // 这个 CPU 也没空闲页，放锁试下一个
    }
  }

  // 恢复中断（与函数开头的 push_off 配对）
  pop_off();

  if(r)
    memset((char*)r, 5, PGSIZE); // 把页填成垃圾值 5，帮助发现使用未初始化内存的 bug
  return (void*)r;               // 返回分配到的页地址；没找到则返回 0
}
