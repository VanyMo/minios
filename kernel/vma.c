//
// Support for memory-mapped files: VMAs, lazy page-fault filling,
// munmap, and write-back of MAP_SHARED pages.
//

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"

// Find the VMA containing va, or 0.
struct vma*
vma_find(struct proc *p, uint64 va)
{
  struct vma *v;

  for(v = p->vmas; v < &p->vmas[NVMA]; v++){
    if(v->used && va >= v->addr && va < v->addr + v->length)
      return v;
  }
  return 0;
}

// Handle a page fault at va by allocating a page and filling it
// from the mapped file. Returns 0 on success, -1 if va is not in
// a VMA or if resources are exhausted.
int
vma_fault(uint64 va)
{
  struct proc *p = myproc();
  struct vma *v;
  uint64 pa, off;
  int perm;

  va = PGROUNDDOWN(va);
  if((v = vma_find(p, va)) == 0)
    return -1;

  if((pa = (uint64)kalloc()) == 0)
    return -1;
  memset((void *)pa, 0, PGSIZE);

  off = v->offset + (va - v->addr);
  if(off < v->f->ip->size){
    begin_op();
    ilock(v->f->ip);
    readi(v->f->ip, 0, pa, off, PGSIZE);
    iunlock(v->f->ip);
    end_op();
  }

  perm = PTE_U;
  if(v->prot & PROT_READ)
    perm |= PTE_R;
  if(v->prot & PROT_WRITE)
    perm |= PTE_W;
  if(mappages(p->pagetable, va, PGSIZE, pa, perm) < 0){
    kfree((void *)pa);
    return -1;
  }
  return 0;
}

// Write back the resident pages in [start, end) of v to the file,
// if the mapping is shared and writable.
static void
vma_writeback(struct proc *p, struct vma *v, uint64 start, uint64 end)
{
  struct inode *ip = v->f->ip;
  uint64 va;

  if(!(v->flags & MAP_SHARED) || !(v->prot & PROT_WRITE))
    return;

  begin_op();
  ilock(ip);
  for(va = start; va < end; va += PGSIZE){
    pte_t *pte = walk(p->pagetable, va, 0);
    uint64 off, n;

    if(pte == 0 || (*pte & PTE_V) == 0)
      continue;
    off = v->offset + (va - v->addr);
    if(off >= ip->size)
      continue;
    n = PGSIZE;
    if(off + n > ip->size)
      n = ip->size - off;
    writei(ip, 1, va, off, n);
  }
  iunlock(ip);
  end_op();
}

// Unmap [addr, addr+len). Handles removing the whole, the start,
// or the end of a VMA. Ranges with no VMA are ignored.
// Returns 0 on success.
int
vma_unmap(uint64 addr, uint64 len)
{
  struct proc *p = myproc();
  uint64 start = PGROUNDDOWN(addr);
  uint64 end = PGROUNDUP(addr + len);

  while(start < end){
    struct vma *v = vma_find(p, start);
    uint64 vend, ustart, uend;

    if(v == 0){
      start += PGSIZE;
      continue;
    }
    vend = v->addr + v->length;
    ustart = (start < v->addr) ? v->addr : start;
    uend = (end > vend) ? vend : end;

    vma_writeback(p, v, ustart, uend);

    uvmunmap(p->pagetable, ustart, (uend - ustart) / PGSIZE, 1);

    if(ustart == v->addr && uend == vend){
      fileclose(v->f);
      v->f = 0;
      v->used = 0;
    } else if(ustart == v->addr){
      v->addr = uend;
      v->length = vend - uend;
    } else {
      v->length = ustart - v->addr;
    }
    start = uend;
  }
  return 0;
}

// Unmap all of the process's VMAs, as if munmap had been called
// on each. Called from exit().
void
vma_free_all(struct proc *p)
{
  struct vma *v;

  for(v = p->vmas; v < &p->vmas[NVMA]; v++){
    if(!v->used)
      continue;
    vma_writeback(p, v, v->addr, v->addr + v->length);
    uvmunmap(p->pagetable, v->addr, v->length / PGSIZE, 1);
    fileclose(v->f);
    v->f = 0;
    v->used = 0;
  }
}
