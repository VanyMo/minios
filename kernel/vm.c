#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"

/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

extern char etext[];  // kernel.ld sets this to end of kernel code.

extern char trampoline[]; // trampoline.S

// Make a direct-map page table for the kernel.
pagetable_t
kvmmake(void)
{
  pagetable_t kpgtbl;

  kpgtbl = (pagetable_t) kalloc();
  memset(kpgtbl, 0, PGSIZE);

  // uart registers
  kvmmap(kpgtbl, UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmap(kpgtbl, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

  // PLIC
  kvmmap(kpgtbl, PLIC, PLIC, 0x400000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap(kpgtbl, KERNBASE, KERNBASE, (uint64)etext-KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap(kpgtbl, (uint64)etext, (uint64)etext, PHYSTOP-(uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap(kpgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

  // allocate and map a kernel stack for each process.
  proc_mapstacks(kpgtbl);
  
  return kpgtbl;
}

// Initialize the one kernel_pagetable
void
kvminit(void)
{
  kernel_pagetable = kvmmake();
}

// Switch h/w page table register to the kernel's page table,
// and enable paging.
void
kvminithart()
{
  // wait for any previous writes to the page table memory to finish.
  sfence_vma();

  w_satp(MAKE_SATP(kernel_pagetable));

  // flush stale entries from the TLB.
  sfence_vma();
}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// The risc-v Sv39 scheme has three levels of page-table
// pages. A page-table page contains 512 64-bit PTEs.
// A 64-bit virtual address is split into five fields:
//   39..63 -- must be zero.
//   30..38 -- 9 bits of level-2 index.
//   21..29 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..11 -- 12 bits of byte offset within the page.
pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc)
{
  if(va >= MAXVA)
    panic("walk");

  for(int level = 2; level > 0; level--) {
    pte_t *pte = &pagetable[PX(level, va)];
    if(*pte & PTE_V) {
      pagetable = (pagetable_t)PTE2PA(*pte);
    } else {
      if(!alloc || (pagetable = (pde_t*)kalloc()) == 0)
        return 0;
      memset(pagetable, 0, PGSIZE);
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
  return &pagetable[PX(0, va)];
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  uint64 pa;

  if(va >= MAXVA)
    return 0;

  pte = walk(pagetable, va, 0);
  if(pte == 0)
    return 0;
  if((*pte & PTE_V) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  pa = PTE2PA(*pte);
  return pa;
}

// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
void
kvmmap(pagetable_t kpgtbl, uint64 va, uint64 pa, uint64 sz, int perm)
{
  if(mappages(kpgtbl, va, sz, pa, perm) != 0)
    panic("kvmmap");
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa.
// va and size MUST be page-aligned.
// Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
int
mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
  uint64 a, last;
  pte_t *pte;

  if((va % PGSIZE) != 0)
    panic("mappages: va not aligned");

  if((size % PGSIZE) != 0)
    panic("mappages: size not aligned");

  if(size == 0)
    panic("mappages: size");
  
  a = va;
  last = va + size - PGSIZE;
  for(;;){
    if((pte = walk(pagetable, a, 1)) == 0)
      return -1;
    if(*pte & PTE_V)
      panic("mappages: remap");
    *pte = PA2PTE(pa) | perm | PTE_V;
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// Remove npages of mappings starting from va. va must be
// page-aligned. The mappings must exist.
// Optionally free the physical memory.
void
uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  uint64 a;
  pte_t *pte;

  if((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");

  for(a = va; a < va + npages*PGSIZE; a += PGSIZE){
    if((pte = walk(pagetable, a, 0)) == 0)
      panic("uvmunmap: walk");
    if((*pte & PTE_V) == 0)
      panic("uvmunmap: not mapped");
    if(PTE_FLAGS(*pte) == PTE_V)
      panic("uvmunmap: not a leaf");
    if(do_free){
      uint64 pa = PTE2PA(*pte);
      kfree((void*)pa);
    }
    *pte = 0;
  }
}

// create an empty user page table.
// returns 0 if out of memory.
pagetable_t
uvmcreate()
{
  pagetable_t pagetable;
  pagetable = (pagetable_t) kalloc();
  if(pagetable == 0)
    return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// Load the user initcode into address 0 of pagetable,
// for the very first process.
// sz must be less than a page.
void
uvmfirst(pagetable_t pagetable, uchar *src, uint sz)
{
  char *mem;

  if(sz >= PGSIZE)
    panic("uvmfirst: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  mappages(pagetable, 0, PGSIZE, (uint64)mem, PTE_W|PTE_R|PTE_X|PTE_U);
  memmove(mem, src, sz);
}

// Allocate PTEs and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz, int xperm)
{
  char *mem;
  uint64 a;

  if(newsz < oldsz)
    return oldsz;

  oldsz = PGROUNDUP(oldsz);
  for(a = oldsz; a < newsz; a += PGSIZE){
    mem = kalloc();
    if(mem == 0){
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if(mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_R|PTE_U|xperm) != 0){
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  if(newsz >= oldsz)
    return oldsz;

  if(PGROUNDUP(newsz) < PGROUNDUP(oldsz)){
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
void
freewalk(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];
    if((pte & PTE_V) && (pte & (PTE_R|PTE_W|PTE_X)) == 0){
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    } else if(pte & PTE_V){
      panic("freewalk: leaf");
    }
  }
  kfree((void*)pagetable);
}

// Free user memory pages,
// then free page-table pages.
void
uvmfree(pagetable_t pagetable, uint64 sz)
{
  if(sz > 0)
    uvmunmap(pagetable, 0, PGROUNDUP(sz)/PGSIZE, 1);
  freewalk(pagetable);
}

// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
//
// =============================================================
// COW 版本：fork 时不真正分配新页，而是“共享”
// =============================================================
//
// 旧版 uvmcopy 在 fork 时为子进程每个用户页都 kalloc 一块新物理页，
// 然后 memmove 把父进程内容拷过去。COW 版本改为：
//   1) 找到父进程第 i 字节所在页的 PTE；
//   2) 让子进程的 PTE 也指向同一物理页；
//   3) 把 PTE_W 清零、PTE_RSW 置位（表示这是一个 COW 共享页）；
//   4) 把该物理页的引用计数 +1（kref_inc），
//      因为现在有 2 个进程都引用它。
//
// 这样 fork() 几乎是“瞬间”完成的。当任何一边真正写入时，CPU 触发
// scause==15 的 page fault，usertrap 调用 cowcopy 给本进程
// 分配一个独立的可写副本。
//
// 注意：原来就不可写的页（如 .text） PTE_W 已经是 0，但仍
// 要把 PTE_RSW 也设上吗？我们不设——这样一来，如果程序尝试写
// 文本段，usertrap 看到 PTE_W==0 且 !PTE_RSW，就认为这是真的
// “写只读段”的 bug，setkilled 即可。
int
uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  pte_t *pte;
  uint64 pa, i;
  uint flags;

  // 遍历父进程所有用户页（按 4K 步进）
  for(i = 0; i < sz; i += PGSIZE){
    // 找到父进程第 i 字节所在页的 PTE
    if((pte = walk(old, i, 0)) == 0)
      panic("uvmcopy: pte should exist");
    // 必须有效
    if((*pte & PTE_V) == 0)
      panic("uvmcopy: page not present");
    // 取出物理页地址
    pa = PTE2PA(*pte);
    // 取出原 PTE 的标志位（R/W/X/U...）
    flags = PTE_FLAGS(*pte);

    // ------- COW 关键改造 -------
    // 如果原页可写，把它改成"只读 + COW"。
    // 不可写的页保持原样（flags 中 PTE_W 已经是 0）。
    if(flags & PTE_W){
      flags = (flags & ~PTE_W) | PTE_RSW;
    }
    // 把子进程的对应虚拟地址映射到同一物理页。
    // mappages 只分配中间层页表页（L1/L0），不分配数据页。
    if(mappages(new, i, PGSIZE, pa, flags) != 0){
      // 失败回滚：撤销已经映射的页（do_free=1 会 kfree）
      uvmunmap(new, 0, i / PGSIZE, 1);
      return -1;
    }
    // 父子两个进程都引用此物理页，引用计数 +1
    kref_inc((void *)pa);
  }
  return 0;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
void
uvmclear(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;

  pte = walk(pagetable, va, 0);
  if(pte == 0)
    panic("uvmclear");
  *pte &= ~PTE_U;
}

// ----------------------------------------------------------------------
// cowcopy(pagetable_t pagetable, pte_t *pte) —— COW page-fault 处理
// ----------------------------------------------------------------------
//
// 这是 COW 的核心实现。被 usertrap() 在发生 store/AMO page fault
// (scause==15) 时调用，前提条件是 *pte 已经被检测为 COW 共享页
// (PTE_V && !PTE_W && PTE_RSW)。
//
// 工作步骤：
//   1) 取出 *pte 当前指向的旧物理页地址 pa；
//   2) kalloc 一块新的物理页 mem；
//   3) memmove(mem, pa, PGSIZE) 把旧页内容整页拷过去；
//   4) kfree(pa) 把旧页的引用计数 -1；
//      （如果父子都引用，refcount 从 2 -> 1，旧页仍属于另一进程；
//       如果只有本进程引用，refcount 从 1 -> 0，旧页归还到 freelist）
//   5) 修改 *pte：PA 指向新页，加上 PTE_W，清掉 PTE_RSW；
//   6) 返回 0 成功；-1 失败（通常是 kalloc 返回 0，内存不足）。
//
// 注意：原 *pte 中保留的 flags (R/U/...) 不会丢失，
// 重新写回时只是调整 PTE_W / PTE_RSW 两个位。
// ----------------------------------------------------------------------
int
cowcopy(pagetable_t pagetable, pte_t *pte)
{
  // 取出旧物理页的物理地址
  uint64 pa = PTE2PA(*pte);
  // 取出 PTE 的标志位（R/W/X/U/RSW...）
  uint flags = PTE_FLAGS(*pte);

  // 申请一个全新的物理页
  // kalloc 内部已经把它的 pa_refcnt 设为 1
  char *mem = kalloc();
  if(mem == 0)
    return -1;

  // 把旧页内容整页拷到新页
  memmove(mem, (char *)pa, PGSIZE);

  // 旧页的引用计数 -1
  // kfree 在 refcount>0 时不会真正把页放回 freelist
  kfree((void *)pa);

  // 改写 PTE：
  //   - PA 部分替换为新物理页地址
  //   - flags 部分保留 R/U/... 但加上 PTE_W、清掉 PTE_RSW
  flags = (flags | PTE_W) & ~PTE_RSW;
  *pte = PA2PTE((uint64)mem) | flags;

  return 0;
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
//
// =============================================================
// COW 改造：copyout 也要处理 COW 共享页
// =============================================================
//
// copyout 把内核缓冲区的内容写到用户地址 dstva 起 len 个字节。
// 旧版逻辑：遇到 PTE_W==0 的页就返回 -1。
//
// 但是在 COW 模式下，一个原来可写的页可能因为 fork 被改成
// “PTE_W==0 && PTE_RSW==1”。内核要能向其中写（如文件系统
// write、pipe write）就必须把它升级为独立可写的物理页。
//
// 解决方法和 usertrap 里处理 store page fault 一模一样：
//   - kalloc 一块新物理页；
//   - 把旧页内容 memmove 过去；
//   - 旧页引用计数 -1（kfree 自动处理）；
//   - 重写 PTE：指向新页、恢复 PTE_W、清掉 PTE_RSW。
//
// 别的进程持有的旧 PTE 仍然指向旧物理页，不会被影响。
int
copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;
  pte_t *pte;

  // 按页处理 src -> dstva
  while(len > 0){
    // 把 dstva 向下取整到页边界
    va0 = PGROUNDDOWN(dstva);
    if(va0 >= MAXVA)
      return -1;

    // 找到 va0 对应的 PTE
    pte = walk(pagetable, va0, 0);
    // 必须是有效、用户可访问的页
    if(pte == 0 || (*pte & PTE_V) == 0 || (*pte & PTE_U) == 0)
      return -1;

    // ------- COW 关键改造 -------
    // 如果这是一个 COW 共享页（PTE_W==0 且 PTE_RSW==1），
    // 就分配一个独立的新物理页。
    if((*pte & PTE_W) == 0 && (*pte & PTE_RSW)){
      pa0 = PTE2PA(*pte);          // 旧物理页
      char *mem = kalloc();         // 新物理页
      if(mem == 0)
        return -1;                  // 内存不足
      // 把旧页的内容整页拷到新页
      memmove(mem, (char *)pa0, PGSIZE);

      // 旧页的引用计数 -1。kfree 在 refcount>0 时不会真正释放。
      kfree((void *)pa0);

      // 重新填写当前 PTE：指向新页、加上 PTE_W、清掉 PTE_RSW
      // PTE_FLAGS 取出原 flags（R/U/...），再调整
      uint flags = PTE_FLAGS(*pte);
      flags = (flags | PTE_W) & ~PTE_RSW;
      *pte = PA2PTE((uint64)mem) | flags;

      // 后面把数据拷到新页
      pa0 = (uint64)mem;
    } else if((*pte & PTE_W) == 0){
      // 真的只读页（不是 COW），例如 .text，不能写
      return -1;
    } else {
      // 普通的可写页
      pa0 = PTE2PA(*pte);
    }

    // 计算本次能写多少字节（不能超过当前页的剩余空间、不能超过 len）
    n = PGSIZE - (dstva - va0);
    if(n > len)
      n = len;
    // 把 src 的 n 字节拷到 pa0 + 偏移 处
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
int
copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
  uint64 n, va0, pa0;

  while(len > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);
    if(n > len)
      n = len;
    memmove(dst, (void *)(pa0 + (srcva - va0)), n);

    len -= n;
    dst += n;
    srcva = va0 + PGSIZE;
  }
  return 0;
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int
copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
  uint64 n, va0, pa0;
  int got_null = 0;

  while(got_null == 0 && max > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);
    if(n > max)
      n = max;

    char *p = (char *) (pa0 + (srcva - va0));
    while(n > 0){
      if(*p == '\0'){
        *dst = '\0';
        got_null = 1;
        break;
      } else {
        *dst = *p;
      }
      --n;
      --max;
      p++;
      dst++;
    }

    srcva = va0 + PGSIZE;
  }
  if(got_null){
    return 0;
  } else {
    return -1;
  }
}
