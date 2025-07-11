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

// 每个CPU都有自己的空闲列表和锁
struct kmem_cpu {
  struct spinlock lock;
  struct run *freelist;
  char name[16]; // 锁的名称
};

struct kmem_cpu kmems[NCPU];

void
kinit()
{
  char name[16];
  for(int i = 0; i < NCPU; i++) {
    snprintf(name, 16, "kmem%d", i);
    initlock(&kmems[i].lock, name);
    kmems[i].freelist = 0;
  }
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

  // 关闭中断并获取当前CPU ID
  push_off();
  int id = cpuid();

  acquire(&kmems[id].lock);
  r->next = kmems[id].freelist;
  kmems[id].freelist = r;
  release(&kmems[id].lock);

  pop_off();
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  // 关闭中断并获取当前CPU ID
  push_off();
  int id = cpuid();

  acquire(&kmems[id].lock);
  r = kmems[id].freelist;
  if(r)
    kmems[id].freelist = r->next;
  release(&kmems[id].lock);

  // 如果当前CPU的空闲列表为空，尝试从其他CPU偷取内存
  if(r == 0) {
    // 遍历所有其他CPU
    for(int i = 0; i < NCPU; i++) {
      if(i == id) continue; // 跳过当前CPU
      
      acquire(&kmems[i].lock);
      r = kmems[i].freelist;
      if(r) {
        // 找到了可用的内存页，偷取它
        kmems[i].freelist = r->next;
        release(&kmems[i].lock);
        break;
      }
      release(&kmems[i].lock);
    }
  }
  
  pop_off();

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}