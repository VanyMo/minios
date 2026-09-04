// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.
//
// ====== COW 实验扩展：每页一个引用计数 ======
// 原本 kalloc/kfree 只是维护一个 freelist；为了支持 copy-on-write，
// 我们需要记录每一页被多少个进程的页表引用，因为只有在最后一个
// 引用消失时才能把页真正放回 freelist。
//
// 实现方式：
//   - 用一个静态数组 pa_refcnt[] 记录每一页的引用计数。
//   - 数组下标 = (页物理地址 - PA0) / PGSIZE，其中 PA0 是我们关心的
//     最小物理地址（end 符号，即内核映像末尾）。
//   - 这样索引值 <= (PHYSTOP - PA0) / PGSIZE，不会越界。

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

// run 是 freelist 链表中一个空闲页的链表节点：把整个 4KB 内存当链表节点用。
struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

// ------- COW 引用计数 -------
// pa_refcnt[i] 表示物理页 (PA0 + i*PGSIZE) 当前被多少个用户页表项引用。
//
// PA0 选 end：内核自己使用的内存（end 以下）不需要被引用计数管理，
// 所以我们只关心从 end 到 PHYSTOP 之间的物理页。
//
// 因为 end 是链接期确定的符号（不是编译期常量），C 不允许用
// 表达式定义一个文件作用域的数组大小。所以我们用 PHYSTOP - KERNBASE
// 这一固定上界（最大物理内存 128MB / 4KB = 32K 页）来开数组。
// 下标转换时做一次范围检查。
#define PA_TOP PHYSTOP
static int pa_refcnt[(PA_TOP - KERNBASE) / PGSIZE];

// 把一个物理地址转换为 pa_refcnt 数组的下标。
// 调用前应确保 pa 落在 [KERNBASE, PHYSTOP) 区间。
static int
pa2index(uint64 pa)
{
  return (pa - KERNBASE) / PGSIZE;
}

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
//
// =============================================================
// COW 版本：每页带引用计数
// =============================================================
//
// 旧版 kfree 只是把 pa 简单地放回 freelist。
// 在 COW 模式下，pa 可能同时被多个进程的页表项引用（fork 后父子
// 共享同一物理页），我们不能直接把它放回 freelist——否则其他进程
// 再去用就会读到已经被 kalloc 出去的“新内容”，错乱。
//
// 因此我们每页维护一个 pa_refcnt[idx]：
//   - kalloc 时设为 1（仅当前进程持有）
//   - uvmcopy 共享给子进程时 kref_inc(pa)，+1
//   - 任何一次 kfree(pa) 都 -1；只有当减到 0 时才真正把页放回 freelist
//
// 这种"最后一引用消失时才回收"是引用计数的核心思想。
void
kfree(void *pa)
{
  struct run *r;

  // 参数合法性检查：pa 必须 4K 对齐，且在 [end, PHYSTOP) 范围内
  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // ------- 引用计数处理 -------
  // 把 kmem.lock 锁住，期间修改 pa_refcnt[idx] 是原子操作，
  // 防止多 CPU 同时释放/增加同一页。
  acquire(&kmem.lock);

  // 把物理地址转换成数组下标
  int idx = pa2index((uint64)pa);

  // 如果 refcount 已经为 0（kinit/freerange 期间），就不要再减，
  // 否则会减到 -1 导致后面“被引用”的判断错乱。
  if(pa_refcnt[idx] > 0)
    pa_refcnt[idx]--;

  // 减完后仍然 >0，说明还有其他进程在用这页——直接返回，
  // 不把它放回 freelist。
  if(pa_refcnt[idx] > 0){
    release(&kmem.lock);
    return;
  }
  release(&kmem.lock);

  // ------- 真正归还到 freelist -------
  // 用 1 填充整页，类似于原来的“junk”，能在某些 bug 场景下
  // 让我们观察到“使用了未初始化内存”的现象。
  memset(pa, 1, PGSIZE);

  // 把整页当成 struct run 链表节点用
  r = (struct run*)pa;
  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

// kref：返回 pa 对应物理页当前的引用计数。
// vm.c 中可以用来检查某页是否被多个进程共享。
int
kref(void *pa)
{
  return pa_refcnt[pa2index((uint64)pa)];
}

// kref_inc：把 pa 的引用计数 +1。
// 在 uvmcopy 把一个父进程的物理页“共享”给子进程时调用，
// 表示多了一个进程引用这页。
void
kref_inc(void *pa)
{
  acquire(&kmem.lock);
  pa_refcnt[pa2index((uint64)pa)]++;
  release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
//
// COW 版本：从 freelist 取下一页之后，把它的 refcount 设为 1
// （只有当前调用 kalloc 的进程会引用它）。
void *
kalloc(void)
{
  struct run *r;

  // 从 freelist 头部取下一页
  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  if(r){
    // 用 5 填充整页（不同于 kfree 的 1），方便观察“从未被使用”的内存
    memset((char*)r, 5, PGSIZE);
    // 引用计数设为 1
    pa_refcnt[pa2index((uint64)r)] = 1;
  }
  return (void*)r;
}
