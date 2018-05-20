#include "param.h"
#include "types.h"
#include "defs.h"
#include "x86.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "elf.h"

extern char data[];  // defined by kernel.ld
pde_t *kpgdir;  // for use in scheduler()

// Set up CPU's kernel segment descriptors.
// Run once on entry on each CPU.
void
seginit(void)
{
  struct cpu *c;

  // Map "logical" addresses to virtual addresses using identity map.
  // Cannot share a CODE descriptor for both kernel and user
  // because it would have to have DPL_USR, but the CPU forbids
  // an interrupt from CPL=0 to DPL=3.
  c = &cpus[cpuid()];
  c->gdt[SEG_KCODE] = SEG(STA_X|STA_R, 0, 0xffffffff, 0);
  c->gdt[SEG_KDATA] = SEG(STA_W, 0, 0xffffffff, 0);
  c->gdt[SEG_UCODE] = SEG(STA_X|STA_R, 0, 0xffffffff, DPL_USER);
  c->gdt[SEG_UDATA] = SEG(STA_W, 0, 0xffffffff, DPL_USER);
  lgdt(c->gdt, sizeof(c->gdt));
}

// Return the address of the PTE in page table pgdir
// that corresponds to virtual address va.  If alloc!=0,
// create any required page table pages.
static pte_t *
walkpgdir(pde_t *pgdir, const void *va, int alloc)
{
  pde_t *pde;
  pte_t *pgtab;
  //page directory entry
  pde = &pgdir[PDX(va)];                    //get the correct entry in the page DIR (shift right 22 and use & )
  if(*pde & PTE_P){                         //if index is not 0, and PRESENT
    pgtab = (pte_t*)P2V(PTE_ADDR(*pde));    //get 20 leftmost bits of page dir entry,and add kernbase
                //TODO: check why P2V?
  } else {
    if(!alloc || (pgtab = (pte_t*)kalloc()) == 0)
      return 0;
    // Make sure all those PTE_P bits are zero.
    memset(pgtab, 0, PGSIZE);
    // The permissions here are overly generous, but they can
    // be further restricted by the permissions in the page table
    // entries, if necessary.
    *pde = V2P(pgtab) | PTE_P | PTE_W | PTE_U;
  }
  return &pgtab[PTX(va)];   //address of page table ENTRY!
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa. va and size might not
// be page-aligned.
//    if(mappages(pgdir, (char*)a, PGSIZE, V2P(mem), PTE_W|PTE_U) < 0){

static int
mappages(pde_t *pgdir, void *va, uint size, uint pa, int perm)
{
  char *a, *last;
  pte_t *pte;

  a = (char*)PGROUNDDOWN((uint)va);
  last = (char*)PGROUNDDOWN(((uint)va) + size - 1);
  for(;;){
    if((pte = walkpgdir(pgdir, a, 1)) == 0)
      return -1;
    if(*pte & PTE_P)
      panic("remap");
    *pte = pa | perm | PTE_P;
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// There is one page table per process, plus one that's used when
// a CPU is not running any process (kpgdir). The kernel uses the
// current process's page table during system calls and interrupts;
// page protection bits prevent user code from using the kernel's
// mappings.
//
// setupkvm() and exec() set up every page table like this:
//
//   0..KERNBASE: user memory (text+data+stack+heap), mapped to
//                phys memory allocated by the kernel
//   KERNBASE..KERNBASE+EXTMEM: mapped to 0..EXTMEM (for I/O space)
//   KERNBASE+EXTMEM..data: mapped to EXTMEM..V2P(data)
//                for the kernel's instructions and r/o data
//   data..KERNBASE+PHYSTOP: mapped to V2P(data)..PHYSTOP,
//                                  rw data + free physical memory
//   0xfe000000..0: mapped direct (devices such as ioapic)
//
// The kernel allocates physical memory for its heap and for user memory
// between V2P(end) and the end of physical memory (PHYSTOP)
// (directly addressable from end..P2V(PHYSTOP)).

// This table defines the kernel's mappings, which are present in
// every process's page table.
static struct kmap {
  void *virt;
  uint phys_start;
  uint phys_end;
  int perm;
} kmap[] = {
 { (void*)KERNBASE, 0,             EXTMEM,    PTE_W}, // I/O space
 { (void*)KERNLINK, V2P(KERNLINK), V2P(data), 0},     // kern text+rodata
 { (void*)data,     V2P(data),     PHYSTOP,   PTE_W}, // kern data+memory
 { (void*)DEVSPACE, DEVSPACE,      0,         PTE_W}, // more devices
};

// Set up kernel part of a page table.
pde_t*
setupkvm(void)
{
  pde_t *pgdir;
  struct kmap *k;

  if((pgdir = (pde_t*)kalloc()) == 0)
    return 0;
  memset(pgdir, 0, PGSIZE);
  if (P2V(PHYSTOP) > (void*)DEVSPACE)
    panic("PHYSTOP too high");
  for(k = kmap; k < &kmap[NELEM(kmap)]; k++)
    if(mappages(pgdir, k->virt, k->phys_end - k->phys_start,
                (uint)k->phys_start, k->perm) < 0) {
      freevm(pgdir);
      return 0;
    }
  return pgdir;
}

// Allocate one page table for the machine for the kernel address
// space for scheduler processes.
void
kvmalloc(void)
{
  kpgdir = setupkvm();
  switchkvm();
}

// Switch h/w page table register to the kernel-only page table,
// for when no process is running.
void
switchkvm(void)
{
  lcr3(V2P(kpgdir));   // switch to the kernel page table
}

// Switch TSS and h/w page table to correspond to process p.
void
switchuvm(struct proc *p)
{
  if(p == 0)
    panic("switchuvm: no process");
  if(p->kstack == 0)
    panic("switchuvm: no kstack");
  if(p->pgdir == 0)
    panic("switchuvm: no pgdir");

  pushcli();
  mycpu()->gdt[SEG_TSS] = SEG16(STS_T32A, &mycpu()->ts,
                                sizeof(mycpu()->ts)-1, 0);
  mycpu()->gdt[SEG_TSS].s = 0;
  mycpu()->ts.ss0 = SEG_KDATA << 3;
  mycpu()->ts.esp0 = (uint)p->kstack + KSTACKSIZE;
  // setting IOPL=0 in eflags *and* iomb beyond the tss segment limit
  // forbids I/O instructions (e.g., inb and outb) from user space
  mycpu()->ts.iomb = (ushort) 0xFFFF;
  ltr(SEG_TSS << 3);
  lcr3(V2P(p->pgdir));  // switch to process's address space
  popcli();
}

// Load the initcode into address 0 of pgdir.
// sz must be less than a page.
void
inituvm(pde_t *pgdir, char *init, uint sz)
{
  char *mem;

  if(sz >= PGSIZE)
    panic("inituvm: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  mappages(pgdir, 0, PGSIZE, V2P(mem), PTE_W|PTE_U);
  memmove(mem, init, sz);
}

// Load a program segment into pgdir.  addr must be page-aligned
// and the pages from addr to addr+sz must already be mapped.
int
loaduvm(pde_t *pgdir, char *addr, struct inode *ip, uint offset, uint sz)
{
  uint i, pa, n;
  pte_t *pte;

  if((uint) addr % PGSIZE != 0)
    panic("loaduvm: addr must be page aligned");
  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walkpgdir(pgdir, addr+i, 0)) == 0)
      panic("loaduvm: address should exist");
    pa = PTE_ADDR(*pte);
    if(sz - i < PGSIZE)
      n = sz - i;
    else
      n = PGSIZE;
    if(readi(ip, P2V(pa), offset+i, n) != n)
      return -1;
  }
  return 0;
}

// Allocate page tables and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
int
allocuvm(pde_t *pgdir, uint oldsz, uint newsz)
{
  char *mem;
  uint a;

  if(newsz >= KERNBASE)
    return 0;
  if(newsz < oldsz)
    return oldsz;

  a = PGROUNDUP(oldsz);
  for(; a < newsz; a += PGSIZE){
    mem = kalloc();
    if(mem == 0){
      cprintf("allocuvm out of memory\n");
      deallocuvm(pgdir, newsz, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if(mappages(pgdir, (char*)a, PGSIZE, V2P(mem), PTE_W|PTE_U) < 0){
      cprintf("allocuvm out of memory (2)\n");
      deallocuvm(pgdir, newsz, oldsz);
      kfree(mem);
      return 0;
    }


      //////
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
int
deallocuvm(pde_t *pgdir, uint oldsz, uint newsz)
{
  pte_t *pte;
  uint a, pa;

  if(newsz >= oldsz)
    return oldsz;

  a = PGROUNDUP(newsz);               //align    newsz   to page size
  for(; a  < oldsz; a += PGSIZE){
    pte = walkpgdir(pgdir, (char*)a, 0);
    if(!pte)
      a = PGADDR(PDX(a) + 1, 0, 0) - PGSIZE;
    else if((*pte & PTE_P) != 0){
      pa = PTE_ADDR(*pte);
      if(pa == 0)
        panic("kfree");
      char *v = P2V(pa);
      kfree(v);
      *pte = 0;
    }
  }
  return newsz;
}
// Free a page table and all the physical memory pages
// in the user part.
void
freevm(pde_t *pgdir)
{
  uint i;

  if(pgdir == 0)
    panic("freevm: no pgdir");
  deallocuvm(pgdir, KERNBASE, 0);
  for(i = 0; i < NPDENTRIES; i++){
    if(pgdir[i] & PTE_P){
      char * v = P2V(PTE_ADDR(pgdir[i]));
      kfree(v);
    }
  }
  kfree((char*)pgdir);
}

// Clear PTE_U on a page. Used to create an inaccessible
// page beneath the user stack.
void
clearpteu(pde_t *pgdir, char *uva)
{
  pte_t *pte;

  pte = walkpgdir(pgdir, uva, 0);
  if(pte == 0)
    panic("clearpteu");
  *pte &= ~PTE_U;
}

// Given a parent process's page table, create a copy
// of it for a child.
pde_t*
copyuvm(pde_t *pgdir, uint sz)
{
  pde_t *d;
  pte_t *pte;
  uint pa, i, flags;
  char *mem;

  if((d = setupkvm()) == 0)
    return 0;
  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walkpgdir(pgdir, (void *) i, 0)) == 0)
      panic("copyuvm: pte should exist");
    if(!(*pte & PTE_P))
      panic("copyuvm: page not present");
    pa = PTE_ADDR(*pte);
    flags = PTE_FLAGS(*pte);
    if((mem = kalloc()) == 0)
      goto bad;
    memmove(mem, (char*)P2V(pa), PGSIZE);
    if(mappages(d, (void*)i, PGSIZE, V2P(mem), flags) < 0)
      goto bad;
  }
  return d;

bad:
  freevm(d);
  return 0;
}

//PAGEBREAK!
// Map user virtual address to kernel address.
char*
uva2ka(pde_t *pgdir, char *uva)
{
  pte_t *pte;

  pte = walkpgdir(pgdir, uva, 0);
  if((*pte & PTE_P) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  return (char*)P2V(PTE_ADDR(*pte));
}

// Copy len bytes from p to user address va in page table pgdir.
// Most useful when pgdir is not the current page table.
// uva2ka ensures this only works for PTE_U pages.
int
copyout(pde_t *pgdir, uint va, void *p, uint len)
{
  char *buf, *pa0;
  uint n, va0;

  buf = (char*)p;
  while(len > 0){
    va0 = (uint)PGROUNDDOWN(va);
    pa0 = uva2ka(pgdir, (char*)va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (va - va0);
    if(n > len)
      n = len;
    memmove(pa0 + (va - va0), buf, n);
    len -= n;
    buf += n;
    va = va0 + PGSIZE;
  }
  return 0;
}











//return 1 if the flag FLAG of page 'vaddr' is SET.
int
isFlagged(struct proc *p,void* vaddr ,uint FLAG){
   pte_t * pte=walkpgdir(p->pgdir,vaddr, 0);
    if((FLAG & *pte) > 0)
      return 1;
    return 0;
}
void
setPTE_FLAG(struct proc *p, const void* vadd, uint FLAG){
  //get the entry
  pte_t * entry=walkpgdir(p->pgdir,vadd, 0);
  if(entry == 0)
    panic("setPTE_PG");
  *entry |= FLAG;   //clear the  flag
}
void
clearPTE_FLAG(struct proc *p, const void* vadd, uint FLAG){
  //get the entry
  pte_t * entry=walkpgdir(p->pgdir,vadd, 0);
  if(entry == 0)
    panic("setPTE_PG");
  *entry &= ~FLAG;   //clear the  flag
}
//page in vaddr. if needed, page out some other one.
int
safe_page_in(struct proc *p, void* vaddr){

}

// page out N different pages into the Back.
int
page_out_N(struct proc *p,int N){
  cprintf("page_out_N",N);
  void* vaddr[N];
  int found_in_ram=0;   //for debugging
  struct page* pages=p->paging_meta->pages;
  int i;
  //find N pages to page OUT
  for(i=0; i<N; i++){
    if(pages[i].exists  && !pages[i].in_back){  //if the page exists and is IN RAM
      vaddr[i] = pages[i].vaddr;                 //save his virtual address in the array
      found_in_ram++;
    }
  }
  if(found_in_ram < N)
    panic("page_out_N");
  //page OUT the pages you found
  for(i=0; i<N; i++){
    if(!pageOut(p,vaddr[i]))
      panic("couldnt pageout");
  }
  return N;
  }


//  get a page from the back, write it into buffer
//  vaddr - of the page
//  return 0 on error
int
getPageFromBack(struct proc* p, const void* vaddr, char* buffer){
  struct page* pages=p->paging_meta->pages;
  int i;
  for(i=0; i  < MAX_TOTAL_PAGES; i++){
    if (pages[i].exists && pages[i].vaddr  ==   vaddr){
      uint offset   = i * PGSIZE;
      if(readFromSwapFile(p,buffer,offset,PGSIZE) < 0)
        panic("get page error");
      return 1;
    }
  }
  return 0;
}

//update page meta-data when going to front
int 
page_in_meta(struct proc* p,void* vaddr){
   struct p_meta *meta  =   p->paging_meta;
   struct page *pages   =   meta->pages;
   int i;
   for(i=0; i<MAX_TOTAL_PAGES; i++){
     if(pages[i].vaddr  ==  vaddr){
       pages[i].in_back =   0;                       //mark as "NOT Backed"
       meta->offsets[pages[i].offset / PGSIZE] = 0;  //mark offset as free
       pages[i].age = 0;                             //reset age
       pages[i].age2= 0xffffffff;
       return 1;
     }
   }
   return 0;
}
//adds a page to the meta data of the Process (when a page is added to the back)
//page needs to already exist in the list.
int 
page_out_meta(struct proc *p,void* vaddr,uint offset){
   struct p_meta *meta  =   p->paging_meta;
   struct page *pages   =   meta->pages;
   int i;
   for(i=0; i<MAX_TOTAL_PAGES; i++){
     if(pages[i].vaddr  ==  vaddr){
       pages[i].in_back =   1;              //mark as "Backed"
       pages[i].offset  =   offset;         //store the offset of the page
       meta->offsets[offset / PGSIZE] = 1;  //mark offset as taken
       return 1;
     }
   }
   return 0;
}
//page in 'to_in' and page out 'to_back'.
int
page_in_out(struct proc* p, void* to_in, void* to_back){
  if(pageOut(p,to_back)){
    return pageIn(p,to_in);
  }
  return -1;
}

//  Called only after checking that this page is indeed paged out!
//  And only when there's less than MAX_PSYC pages in memory.
int
pageIn(struct proc *p, void* vaddr){
    char* paddr;    //will contain Physical address that the page would be written to.
    paddr = kalloc();                           //allocate physical page
    mappages(p->pgdir,vaddr,PGSIZE,(uint)paddr,0);    //map the vaddr to the newly allocated Paddr
    if(!getPageFromBack(p,vaddr,vaddr))             //write the page into memory (vaddr is already mapped to paddr)
      return 0;
    clearPTE_FLAG(p,vaddr,PTE_PG);              //clear the PAGED OUT flag
    setPTE_FLAG(p,vaddr,PTE_P);                 //set the PRESENT flag
    
    if(!page_in_meta(p,vaddr))                      //remove meta data from the process meta-data struct 
      return 0;
    return 1;   
}

//returns 1 if the page is PAGED OUT (not present AND marked as paged out.)
int
isPagedOut(struct proc *p,  void* vaddr){
  if( !isFlagged(p,vaddr,PTE_P)  &&  isFlagged(p,vaddr,PTE_PG))
    return 1;
  return 0;
}
//get the next free offset in the Back file
//return PGFILE_FULL_ERR if none found (file is full  - TODO: Possible?!)
uint
getFreePageOffset(struct proc *p){
  struct p_meta *meta=p->paging_meta;
  int i;

  for(i=0; i< MAX_TOTAL_PAGES; i++){
    if(meta -> offsets[i] == 1) //if taken, continue..
      continue;
    return PGSIZE * i ;
  }
  return PGFILE_FULL_ERR ;
}
// adds a page (with address vadd) to the back (the file)
int
addPageToBack(struct proc *p, void* vaddr){
    int free_offset=getFreePageOffset(p);
    if(free_offset == PGFILE_FULL_ERR)
      return 0;
    writeToSwapFile(p,vaddr,free_offset,PGSIZE);    //  write the page to the swap file
    page_out_meta(p,vaddr,free_offset);             //  add to meta-data of the process
   
    return 1;
}

//page out a page with the adderss vaddr
int
pageOut(struct proc *p,void* vaddr){
  char* to_free;

   //write page to the Back file.
  if(addPageToBack(p,vaddr)){
    if(!(to_free=uva2ka(p->pgdir,vaddr)))
      panic("page-out");
    kfree(to_free);                                   //free the PHYSICAL memory of the page
    clearPTE_FLAG(p,vaddr,PTE_P);                     //clear the Present flag from the page table entry
    setPTE_FLAG(p,vaddr,PTE_PG);                      //set the PAGED-OUT flag
    lcr3(V2P(p->pgdir));                              //refresh the Table Lookaside Buffer            
    return 1;
  }
  return 0;   //error adding page to Back
}






//  A Naive way to get some page that we can remove to the Back file.
void*
naive_getPageToStore(struct proc *p){
  struct page *pages=p->paging_meta->pages;
  int i;
  for(i = 0; i< MAX_TOTAL_PAGES; i++){
    if(pages[i].exists && !pages[i].in_back){ // if page #i exists and is currentnly in RAM
      return pages[i].vaddr;
    }
  }
  return (void*)-1;
}

// returns 1 if the process has a place in the back for a page.
int hasPlaceInBack(struct proc *p){
  if(numOfPagedOut(p) < MAX_TOTAL_PAGES)
    return 1;
  return 0;
}
//returns the number of paged out Pages
int
numOfPagedOut(struct proc *p){
  struct page* pages=p->paging_meta->pages;
  int i;
  int counter = 0;
  for(i=0; i  <MAX_TOTAL_PAGES; i++){
    if(pages[i].exists  == 0)
      continue;  
    if(pages[i].in_back ==  1)
      counter ++;
  }
  return counter;  
}


//Number of pages in RAM
int
numOfPagedIn(struct proc *p){
  struct page* pages=p->paging_meta->pages;
  int i;
  int counter = 0;
  for(i=0; i  <MAX_TOTAL_PAGES; i++){
    if(pages[i].exists  == 0)
      continue;  
    if(pages[i].in_back ==  0)
      counter ++;
  }
  return counter;  
}









//copy the parent's swapfile to the child.
//copy only if parent is not the shell or init.
int
copy_parent_swapfile(struct proc *child, struct proc *parent){
    int chunk=  PGSIZE/2;   //limit ? 
    int bytes_read;
    char buff [PGSIZE/2]="";
    int offset=0;
    while((bytes_read=readFromSwapFile(parent,buff,offset,chunk))>0){//read chunk from parent
      writeToSwapFile(child,buff,offset,chunk);                      //write chunk to child
      offset+=bytes_read;                                            //next offset
    }
  
  return 1;
}



int
clearPTE_P(struct proc *p, const void* vadd){
  //get the entry
  pte_t * entry=walkpgdir(p->pgdir,vadd, 0);
  if(entry == 0)
    panic("clearPTE_P");
  *entry &= ~PTE_P;   //clear the P flag
  return 1;
}
int
setPTE_PG(struct proc *p, const void* vadd){
  //get the entry
  pte_t * entry=walkpgdir(p->pgdir,vadd, 0);
  if(entry == 0)
    panic("setPTE_PG");
  *entry |= PTE_P;   //clear the P flag
  return 1;
}





// Adds a TOTALLY new page to the process's list.
int
add_new_page(struct proc *p, void* vaddr){
  struct p_meta *meta = p->paging_meta;
  struct page *pages=meta->pages;
  int i;
  for(i=0; i<MAX_TOTAL_PAGES; i++){
    if(pages[i].exists && pages[i].vaddr == vaddr){
      panic("add_new_page vaddr exists");
    }
    if(pages[i].exists)
      continue;
    pages[i].exists = 1;      //mark this spot as taken, a page exists here now.
    pages[i].vaddr  = vaddr;
    pages[i].in_back= 0;
    pages[i].age  = 0;
    pages[i].age2  = 0xffffffff;
    return 1;
  }
  return 0;
}



//Aging
void
age_process_pages(struct proc* proc){
  struct page * pa=proc->paging_meta->pages;
  int i;
  //for every page
  for(i=0; i<MAX_TOTAL_PAGES; i++){
    pte_t *e= walkpgdir(proc->pgdir,pa[i].vaddr,0);
    if((*e & PTE_A) > 0){            // if accessed
      *e &=~PTE_A;                   // clear Accessed bit
      pa[i].age=pa[i].age / 2;       //shift right
      pa[i].age=pa[i].age | MSB;     //set MSB 
      //for LAPA
      pa[i].age2=pa[i].age2 / 2;     //shift right
      pa[i].age2=pa[i].age2 | MSB;   //set MSB 
      
    }
    else{                             //if not visited 
      pa[i].age=pa[i].age / 2;       //just shift right
      pa[i].age2=pa[i].age2 / 2;       //just shift right
    }
  }

}


//
void*
select_page_to_back(struct proc *p){
  //implement algorithms
  
  #ifdef NFUA
  struct page  min_page;     //vaddr of that page
  int i = 0;
  struct page * pa=p->paging_meta->pages;
  //first loop  - get the first page that exists and is NOT in the back.
  for(i = 0; i<MAX_TOTAL_PAGES; i++){
    if(pa[i].exists && !pa[i].in_back){
        min_page=pa[i];
    }
  }
  //Possible bug?
  for(; i<MAX_TOTAL_PAGES; i++){
    if(!pa[i].exists || pa[i].in_back)  //if spot not occupied OR the page is in the back file already , skip.
      continue;
    if(pa[i].age < min_page.age){
      min_page=pa[i];
    }
  }
  //return this page's vaddr
  return min_page.vaddr;
  #endif


  #ifdef LAPA
  int    min_count;         //min num of set bits
  struct page  min_page;     //vaddr of that page
  int i = 0;
  struct page * pa=p->paging_meta->pages;
  //first loop  - get the first page that exists and is NOT in the back.
  for(i = 0; i<MAX_TOTAL_PAGES; i++){
    if(pa[i].exists && !pa[i].in_back){
        min_count=count_set_bits(pa[i].age2);
        min_page=pa[i];
    }
  }
  //Possible bug?
  for(; i<MAX_TOTAL_PAGES; i++){
    if(!pa[i].exists || pa[i].in_back || pa[i].vaddr  ==  min_page.vaddr)  //if spot not occupied OR the page is in the back file already, skip
      continue;
    int curr_count  = count_set_bits(pa[i].age2);
    if(curr_count < min_count){
      min_count=curr_count;
      min_page=pa[i];
    }
    else if( curr_count == min_count){
      if(pa[i].age2 < min_page.age2){
        min_count=count_set_bits(pa[i].age2);
        min_page=pa[i];
      }
    }
  }
  //return this page's vaddr
  return min_page.vaddr;
  #endif
  #ifdef SCFIFO




  #endif


}
// counter number of 1's in a number
uint 
count_set_bits(uint number){
  uint counter  =   0;
  int i;
  for(i=0; i<32; i++){
    if((number | 1) > 0)
      counter ++;
    number = number / 2;
  }
  return counter;
}


//Enqueue a page
int enqueue(struct proc *pr,struct page toAdd) {
    struct p_meta meta=proc->paging_meta;
    if (meta.pq.lastIndex == MAX_TOTAL_PAGES) {
        //no place
        return 0;
    } else {
        meta.pq.pages[meta.pq.lastIndex] = toAdd;
        meta.pq.lastIndex++;
    }
}

//Dequeue a page
struct page dequeue(struct proc *pr) {
    struct p_meta meta=pr->paging_meta;
    struct page toReturn = meta.pq.pages[0];
    struct page_queue pq=meta.pq;
    pq.pages[0] = 0;
    int i;
    for (i = 0; i < MAX_TOTAL_PAGES; i++) {
        if (i == MAX_TOTAL_PAGES - 1)
            pq.pages[i] = 0;
        else
            pq.pages[i] = pq.pages[i + 1];
    }
    pq.lastIndex--;
    return toReturn;
}



//PAGEBREAK!
// Blank page.
//PAGEBREAK!
// Blank page.
//PAGEBREAK!
// Blank page.

