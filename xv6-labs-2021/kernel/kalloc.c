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
  int freelen;
} kmem[NCPU];

void
kinit()
{
  char lockname[7];
  for(int i = 0; i < NCPU; i++) {
    snprintf(lockname, sizeof(lockname), "kmem_%d", i);
    initlock(&kmem[i].lock, lockname);
    kmem[i].freelen = 0;
  }
  freerange(end, (void*)PHYSTOP);
}

struct run*
reschedule(int id)
{
  if(!holding(&kmem[id].lock))
    panic("rechedule lock error1");

  if(kmem[id].freelist)
    return (struct run*)0;

  int maxid = -1, maxlen = 0;
  for(int i = 0; i < NCPU; i++) {
    if(i == id)
      continue;
    acquire(&kmem[i].lock);
    if(kmem[i].freelen > maxlen) {
      if(maxid != -1)
        release(&kmem[maxid].lock);
      maxlen = kmem[i].freelen;
      maxid = i;
    }
    else
      release(&kmem[i].lock);
  }
  if(maxid == -1)
    return (struct run*)0;

  if(!holding(&kmem[maxid].lock))
    panic("rechedule lock error2");

  struct run *r = kmem[maxid].freelist;

  if(r) {
    struct run *fp = r;
    while(fp && fp->next) {
      fp = fp->next->next;
      r = r->next;
    }
    if(r == kmem[maxid].freelist) {
      kmem[maxid].freelist = 0;
      kmem[maxid].freelen = 0;
      kmem[id].freelist = 0;
      kmem[id].freelen = 0;
      release(&kmem[maxid].lock);
      return r;
    }
    kmem[id].freelist = kmem[maxid].freelist;
    kmem[maxid].freelist = r->next;
    r->next = 0;
    if((kmem[maxid].freelen % 2) == 0) {
      kmem[maxid].freelen = kmem[maxid].freelen / 2 - 1;
      kmem[id].freelen = kmem[maxid].freelen + 1;
    }else {
      kmem[maxid].freelen = kmem[maxid].freelen / 2;
      kmem[id].freelen = kmem[maxid].freelen;
    }
    release(&kmem[maxid].lock);
    r = kmem[id].freelist;
    kmem[id].freelist = r->next;
    return r;
  }

  release(&kmem[maxid].lock);
  return (struct run*)0;
}


void checklen(int id)
{
  struct run *r;
  int len = 0;
  for(int i = 0; i < NCPU; i++)
  {
    if(i != id)
      acquire(&kmem[i].lock);
    r = kmem[i].freelist;
    while(r) {
      r = r->next;
      len ++;
    }
    if(len != kmem[i].freelen) {
      printf("cpu: %d, real: %d, freelen: %d\n", i, len, kmem[i].freelen);
      panic("checklen not match\n");
    }
    if(i != id)
      release(&kmem[i].lock);
    len = 0;
  }
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by v,
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

  push_off();
  int cpu = cpuid();

  acquire(&kmem[cpu].lock);
  r->next = kmem[cpu].freelist;
  kmem[cpu].freelist = r;
  ++kmem[cpu].freelen;
  release(&kmem[cpu].lock);

  pop_off();
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  push_off();
  int cpu = cpuid();

  acquire(&kmem[cpu].lock);
  r = kmem[cpu].freelist;
  if(r) {
    kmem[cpu].freelist = r->next;
    --kmem[cpu].freelen;
  }
  else
    r = reschedule(cpu);

  release(&kmem[cpu].lock);

  pop_off();

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}

