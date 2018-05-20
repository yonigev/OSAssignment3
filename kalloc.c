// Physical memory allocator, intended to allocate
// memory for user processes, kernel stacks, page table pages,
// and pipe buffers. Allocates 4096-byte pages.

#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "spinlock.h"

void freerange(void *vstart, void *vend);
extern char end[]; // first address after kernel loaded from ELF file
                   // defined by the kernel linker script in kernel.ld

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  int use_lock;
  struct run *freelist;
} kmem;

// Initialization happens in two phases.
// 1. main() calls kinit1() while still using entrypgdir to place just
// the pages mapped by entrypgdir on free list.
// 2. main() calls kinit2() with the rest of the physical pages
// after installing a full page table that maps them on all cores.
void
kinit1(void *vstart, void *vend)
{
  initlock(&kmem.lock, "kmem");
  kmem.use_lock = 0;
  freerange(vstart, vend);
}

void
kinit2(void *vstart, void *vend)
{
  freerange(vstart, vend);
  kmem.use_lock = 1;
}

void
freerange(void *vstart, void *vend)
{
  char *p;
  p = (char*)PGROUNDUP((uint)vstart);
  for(; p + PGSIZE <= (char*)vend; p += PGSIZE)
    kfree(p);
}
//PAGEBREAK: 21
// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(char *v)
{

  cprintf("in KFREE start !\n");
  struct run *r;

  if((uint)v % PGSIZE || v < end || V2P(v) >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(v, 1, PGSIZE);         //fill with 1's?

  if(kmem.use_lock)             //lock
    acquire(&kmem.lock);
  r = (struct run*)v;           //"cast" into a run*
  r->next = kmem.freelist;      //make it first in the list
  kmem.freelist = r;            //
  if(kmem.use_lock)             //unlock
    release(&kmem.lock);
    cprintf("in KFREE END !\n");
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
char*
kalloc(void)
{
  cprintf("in kalloc start !\n");
  struct run *r;
  int i=myproc()->pid;
  if(kmem.use_lock)           //lock?
    acquire(&kmem.lock);
  r = kmem.freelist;          //take the list of free pages
  if(r)                       //if not 0
    kmem.freelist = r->next;  //"delete" a page. meaning make the list start from the second free page
  if(kmem.use_lock)
    release(&kmem.lock);
  cprintf("in kalloc end ! (released lock)\n");
  return (char*)r;            //return the page
}
//Returns number of free pages in memory
int
num_free(void){
  int counter=0;
  struct run *r;

  if(kmem.use_lock)           //lock?
    acquire(&kmem.lock);
  r = kmem.freelist;          //take the list of free pages
  while(r){                       //if not 0
    counter ++;
    r = r->next;  
  }
  if(kmem.use_lock)
    release(&kmem.lock);
  return counter;            //return the page
}

