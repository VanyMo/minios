// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

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
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}

// [新增] 统计当前空闲物理内存的总字节数，供 sysinfo 系统调用使用。
//
// 原理：本文件的内存分配器用一条"空闲链表"管理所有空闲页——
// kfree() 把一页挂到链表头，kalloc() 从链表头摘下一页。
// 所以只要从头到尾走一遍这条链表（kmem.freelist），
// 数出有多少个节点（每个节点 = 一个 4096 字节的空闲页），
// 节点数 × PGSIZE 就是空闲内存的总字节数。
//
// 加锁说明：kmem.lock 保护 freelist 不被多个 CPU 同时修改。
// 遍历链表期间必须持有该锁，否则别的 CPU 同时 kalloc/kfree
// 会改变链表结构，导致遍历出错（读到半更新的指针）。
uint64
kfreemem(void)
{
  struct run *r;     // [新增] 遍历链表用的游标指针
  uint64 n = 0;      // [新增] 累加器：统计到的空闲字节数

  acquire(&kmem.lock);          // [新增] 拿住分配器自旋锁，独占访问空闲链表
  for(r = kmem.freelist; r; r = r->next)  // [新增] 从链表头走到链表尾（NULL 结尾）
    n += PGSIZE;                // [新增] 每遇到一个空闲页，累加一页的字节数（4096）
  release(&kmem.lock);          // [新增] 释放锁，让其他 CPU 可以继续分配/释放内存

  return n;         // [新增] 返回空闲内存总字节数
}
