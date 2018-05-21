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
void*   select_page_to_back(struct proc *p);
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

    #ifndef NONE
    //add new page
    if(myproc()){
      int pages_in_ram=numOfPagedIn(myproc());
      if(pages_in_ram == MAX_PSYC_PAGES){
        cprintf("paging out something ...in ram: %d\n",numOfPagedIn(myproc()));
        pageOut(myproc(),select_page_to_back(myproc()));
        cprintf("finished paging out something ...in ram: %d\n",numOfPagedIn(myproc()));
      }
    }
    #endif


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
    #ifndef NONE
    add_new_page(myproc(),(void *)a);
    #endif


      //////
  }
  return newsz;
}


//when freeing memory   -   remove this page from queue
void
free_from_queue(struct proc *p,void* vaddr){
  struct p_meta meta;
  meta=p->paging_meta;
  struct page_queue pq=meta.pq;
  int to_del;     //index of page to delete from queue
  for(to_del = 0; to_del<MAX_TOTAL_PAGES; to_del++){
    if(!(pq.pages[to_del].exists && pq.pages[to_del].vaddr == vaddr))
      continue;
    break;
  }
  int i;
  for(i=to_del; i<pq.lastIndex; i++){
    if(i == MAX_TOTAL_PAGES-1){ //if its the last one, just delete it
      pq.pages[i].exists = 0;
      pq.pages[i].vaddr  = 0;
      pq.pages[i].age    = 0;
      pq.pages[i].age2   = 0;
      pq.pages[i].in_back= 0;
    }
    else
      pq.pages[i] = pq.pages[i + 1];  //shift    
  }
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
      #ifndef NONE
      if(myproc()){
          struct p_meta *meta=&myproc()->paging_meta;
          struct page *pages=meta->pages;
          int i;
          for(i=0; i<MAX_TOTAL_PAGES; i++){
            if(pages[i].exists && pages[i].vaddr ==(void *) a){
              pages[i].age=0;
              pages[i].age2=0xffffffff;
              pages[i].in_back=0;
              pages[i].offset=0;
              pages[i].vaddr=0;
              pages[i].exists=0;
            }
          }
          free_from_queue(myproc(),(void *)a);





      }
      #endif

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


// Our new Functions









//Enqueue a page - is different if defined AQ
int enqueue(struct proc *pr,struct page toAdd) {
    struct p_meta *meta;
    meta=&pr->paging_meta;
    cprintf("Enqueueing   -   %x\n",toAdd.vaddr);
    
    if (meta->pq.lastIndex == MAX_TOTAL_PAGES) {
        //no place
        return 0;
    } else {
        meta->pq.pages[meta->pq.lastIndex] = toAdd;
        meta->pq.lastIndex++;
        return 1;
    }

}

//Dequeue a page  - is different if defined AQ
struct page dequeue(struct proc *pr) {
    struct p_meta meta;
    meta=pr->paging_meta;
    struct page toReturn = meta.pq.pages[0];
    struct page_queue pq=meta.pq;
    pq.pages[0].exists  = 0;          //delete
    pq.pages[0].vaddr   = (void *)0;  //it
    int i;
    for (i = 0; i < pq.lastIndex; i++) {    //CHANGED to pq.lastIndex from MAX_TOTAL_PAGES
        if (i == MAX_TOTAL_PAGES - 1){
            //pq.pages[i] = {0,0,0,0,0,0};
            pq.pages[i].exists = 0;
            pq.pages[i].vaddr  = 0;
            pq.pages[i].age    = 0;
            pq.pages[i].age2   = 0;
            pq.pages[i].in_back= 0;
        }
        else
            pq.pages[i] = pq.pages[i + 1];
    }
    pq.lastIndex--;
    return toReturn;
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



  


//  get a page from the back, write it into buffer
//  vaddr - of the page
//  return 0 on error
int
getPageFromBack(struct proc* p, const void* vaddr, char* buffer){
  struct page* pages=p->paging_meta.pages;
  int i;
  for(i=0; i  < MAX_TOTAL_PAGES; i++){
    if (pages[i].exists && pages[i].vaddr  ==   vaddr && pages[i].in_back){
      uint offset   = pages[i].offset;
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
  cprintf("paging in meta- %x\n",vaddr);
   struct p_meta *meta  =   &p->paging_meta;
   struct page *pages   =   meta->pages;
   int i;
   for(i=0; i<MAX_TOTAL_PAGES; i++){
     if(pages[i].vaddr  ==  vaddr){
       pages[i].in_back =   0;                       //mark as "NOT Backed"
       meta->offsets[pages[i].offset / PGSIZE] = 0;  //mark offset as free
       pages[i].age = 0;                             //reset age
       pages[i].age2= 0xffffffff;
       enqueue(p,pages[i]);                          //enqueue after paging in .
       return 1;
     }
   }
   return 0;
}


//  Called only after checking that this page is indeed paged out!
//  And only when there's less than MAX_PSYC pages in memory.
int
pageIn(struct proc *p, void* vaddr){
    cprintf("in pageIn with: %x\n",vaddr);
    char* paddr;    //will contain Physical address that the page would be written to.
    paddr = kalloc();                           //allocate physical page
    if(mappages(p->pgdir,vaddr,PGSIZE,V2P(paddr),0)!=0)    //map the vaddr to the newly allocated Paddr
      panic("pagein-mappages");

    cprintf("mapped v: %x to p: %x\n",vaddr,V2P(paddr));
    if(!getPageFromBack(p,vaddr,vaddr))             //write the page into memory (vaddr is already mapped to paddr)
      return 0;
    clearPTE_FLAG(p,vaddr,PTE_PG);             //clear the PAGED OUT flag
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
  struct p_meta *meta=&p->paging_meta;
  int i;

  for(i=0; i< MAX_TOTAL_PAGES; i++){
    if(meta -> offsets[i] == 1) //if taken, continue..
      continue;
    return PGSIZE * i ;
  }
  return PGFILE_FULL_ERR ;
}

//adds a page to the meta data of the Process (when a page is added to the back)
//page needs to already exist in the list.
int 
page_out_meta(struct proc *p,void* vaddr,uint offset){
  cprintf("paging out meta:   %x\n",vaddr);
   struct p_meta *meta  =   &p->paging_meta;
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

// adds a page (with address vadd) to the back (the file)
int
addPageToBack(struct proc *p, void* vaddr){
    int free_offset=getFreePageOffset(p);

    if(free_offset == PGFILE_FULL_ERR)
      return 0;
    writeToSwapFile(p,(char *)PTE_ADDR(vaddr),free_offset,PGSIZE);    //  write the page to the swap file
    page_out_meta(p,(char *)PTE_ADDR(vaddr),free_offset);                       //  add to meta-data of the process
   
    return 1;
}

//page out a page with the adderss vaddr
int
pageOut(struct proc *p,void* vaddr){
  cprintf("paging out vaddr: %x\n",vaddr);
  char* to_free;
   //write page to the Back file.
  if(addPageToBack(p,vaddr)){
    pte_t *pte=walkpgdir(p->pgdir,vaddr,0);
    to_free=(char*)P2V(PTE_ADDR(*pte));   
    kfree(to_free);                                   //free the PHYSICAL memory of the page
    clearPTE_FLAG(p,vaddr,PTE_P);                     //clear the Present flag from the page table entry
    setPTE_FLAG(p,vaddr,PTE_PG);                      //set the PAGED-OUT flag
    lcr3(V2P(p->pgdir));                              //refresh the Table Lookaside Buffer   
    p->num_pageouts++;         
    return 1;
  }
  else
    panic("page_out");
  return 0;   //error adding page to Back
}

//returns the number of paged out Pages
int
numOfPagedOut(struct proc *p){
  struct page* pages=p->paging_meta.pages;
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
  struct page* pages=p->paging_meta.pages;
  int i;
  int counter = 0;
  for(i=0; i  <MAX_TOTAL_PAGES; i++){
    if(pages[i].exists  == 0)
      continue;  
    if(pages[i].in_back ==  0 && pages[i].exists == 1)
      counter ++;
  }
  return counter;  
}


//page in a certain page - page-out another if needed
int
safe_page_in(struct proc* p,void *vaddr){
  if(numOfPagedIn(p) == MAX_PSYC_PAGES){
    cprintf("ram full, paging out first-\n");
    pageOut(p,select_page_to_back(p));
  }
  cprintf("calling page in with : %x\n",vaddr);

  return pageIn(p,(void *)PGROUNDDOWN((uint)vaddr));
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
    //copy all the meta data from the parent
    memmove(&child->paging_meta,&parent->paging_meta,sizeof(struct p_meta));
    
  
  return 1;
}

//return number of allocated pages for this process
int
get_allocated_pages(struct proc *p){
  struct page *pages=p->paging_meta.pages;
  int i;
  int counter = 0;
  for(i=0; i<MAX_TOTAL_PAGES; i++){
    if(pages[i].exists){
      counter++;
    }
  }
  return counter;
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

// Adds a TOTALLY new page to the process's list.
int
add_new_page(struct proc *p, void* vaddr){
  cprintf("adding new page: %x, in ram: %d  \n",vaddr,numOfPagedIn(p));
  struct p_meta *meta = &p->paging_meta;
  struct page *pages= meta->pages;
  int i;
  struct page toAdd={.exists = 1, .vaddr = vaddr, .in_back = 0, .age = 0, .age2 = 0xffffffff};

  for(i=0; i<MAX_TOTAL_PAGES; i++){
    if(pages[i].exists && pages[i].vaddr == vaddr){
      panic("add_new_page vaddr exists");
    }
    if(pages[i].exists)
      continue;
    // pages[i].exists = 1;      //mark this spot as taken, a page exists here now.
    // pages[i].vaddr  = vaddr;
    // pages[i].in_back= 0;
    // pages[i].age  = 0;
    // pages[i].age2  = 0xffffffff;
    pages[i]  = toAdd;
    enqueue(p,toAdd);
    




    return 1;
  }
  return 0;
}


//free a page with Virtual Address vaddr
void
free_page(struct proc *p, void* vaddr){
  struct p_meta *meta=&p->paging_meta;
  struct page *pages=meta->pages;
  int i;
  for(i=0; i<MAX_TOTAL_PAGES; i++){
    if(pages[i].exists && pages[i].vaddr == vaddr){
      pages[i].age=0;
      pages[i].age2=0xffffffff;
      pages[i].in_back=0;
      pages[i].offset=0;
      pages[i].vaddr=0;
      pages[i].exists=0;
    }
  }
  free_from_queue(p,vaddr);
}
//Aging
void
age_process_pages(struct proc* proc){
  struct page * pa_arr=proc->paging_meta.pages;
  int i;

  //for every page
  for(i=0; i<MAX_TOTAL_PAGES; i++){
    if(!pa_arr[i].exists  || pa_arr[i].in_back)
    continue;
    pte_t *e= walkpgdir(proc->pgdir,pa_arr[i].vaddr,0);
  
    if((*e & PTE_A) > 0){            // if accessed
      *e &=~PTE_A;                   // clear Accessed bit
      pa_arr[i].age=pa_arr[i].age / 2;       //shift right
      pa_arr[i].age=pa_arr[i].age | MSB;     //set MSB 
      //for LAPA
      pa_arr[i].age2=pa_arr[i].age2 / 2;     //shift right
      pa_arr[i].age2=pa_arr[i].age2 | MSB;   //set MSB 
      
    }
    else{                             //if not visited 
      pa_arr[i].age=pa_arr[i].age / 2;       //just shift right
      pa_arr[i].age2=pa_arr[i].age2 / 2;       //just shift right
    }
  }

#ifdef AQ
  struct page_queue *pq=&proc->paging_meta.pq;   //access the actuall Page Queue
  struct page *pa= pq->pages;                 //access the Queue's array.
  int j;
  //start from the second place from last.
  for(j = pq->lastIndex - 1; j>=0; j--){
    pte_t *entry_j = walkpgdir(proc->pgdir,pa[j].vaddr,0);
    pte_t *entry_prec_j = walkpgdir(proc->pgdir,pa[j+1].vaddr,0);
    //if the i'th page was accessed, and the i-1 not, switch them.
    if((*entry_j & PTE_A) > 0 && (*entry_prec_j & PTE_A)<=0){
      struct page temp=pa[j];
      pa[j] = pa[j+1];
      pa[j+1]=temp;
    }
  }
  //Clean up Accessed bits
  for(j = pq->lastIndex - 1; j>=0; j--){
    pte_t *entry=walkpgdir(proc->pgdir,pa[j].vaddr,0);
    *entry &=~PTE_A;                   // clear Accessed bit
  }  


#endif

}

//returns 1 if it's a user page (and can be swapped out).
//0 otherwise
int
is_user_page(struct proc* p, void* vaddr){
  // pte_t *entry=walkpgdir(p->pgdir,vaddr,0);
  // if((*entry & PTE_U) > 0)
     return 1;

  // return 0;
}

// Returns a Virtual Address of a page to be replaced in the RAM, according to replacement algorithms.
void*
select_page_to_back(struct proc *p){
  //implement algorithms
  
    #ifdef NFUA
    struct page  min_page={.exists = 0, .vaddr=(void *)0, .in_back=0,.offset=0, .age=0,.age2=0};   
    int i = 0;
    struct page * pa  =   (&(p->paging_meta))->pages;
    //first loop  - get the first page that exists and is NOT in the back, and is legal to swap out
    for(i = 0; i<MAX_TOTAL_PAGES; i++){
      if(pa[i].exists && !pa[i].in_back && is_user_page(p,pa[i].vaddr)){
          min_page=pa[i];
         // cprintf("SELECT PAGE : NFUA : FIRST NORMAL : FOUND : %x\n",min_page.vaddr);
      }
    }
    //Possible bug?
    for(; i<MAX_TOTAL_PAGES; i++){
      if(!pa[i].exists || pa[i].in_back || !is_user_page(p,pa[i].vaddr))  //if does not exist, already in back OR not legal to swap out, skip.
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
    struct page  min_page={.exists = 0, .vaddr=(void *)0, .in_back=0,.offset=0, .age=0,.age2=0};   
    int i = 0;
    struct page * pa  =   (&(p->paging_meta))->pages;
    //first loop  - get the first page that exists and is NOT in the back, and legal to swap out
    for(i = 0; i<MAX_TOTAL_PAGES; i++){
      if(pa[i].exists && !pa[i].in_back && is_user_page(p,pa[i].vaddr)){
          min_count=count_set_bits(pa[i].age2);
          min_page=pa[i];
      }
    }
    //Possible bug?
    for(; i<MAX_TOTAL_PAGES; i++){
      if(!pa[i].exists || pa[i].in_back || pa[i].vaddr  ==  min_page.vaddr || !is_user_page(p,pa[i].vaddr))  //as above
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
    struct page current;
    while(1){
      current = dequeue(p);
      pte_t *e= walkpgdir(p->pgdir,current.vaddr,0);  //get the PTE
      if((*e & PTE_A) > 0 || !is_user_page(p,current.vaddr)){              // if accessed OR illegal to swap out
          *e &=~PTE_A;                   // clear Accessed bit
          enqueue(p,current);            // give second chance
      }
      else{                              //if not accessed
        return current.vaddr;
      }
    }
    #endif

    #ifdef AQ
    while(1){
      struct page toReturn;
      toReturn  = dequeue(p);
      //if it's legal to swap out, do it. otherwise, keep looping.
      if(is_user_page(p,toReturn.vaddr))   
        return toReturn.vaddr;
      else
        enqueue(p,toReturn);
    }
    #endif
  #ifdef NONE
  return (void*) 1;   //delete
  #endif
}






void
reset_paging_meta(struct proc* pr){
   memset(&pr->paging_meta,0,sizeof(struct p_meta));
}

//PAGEBREAK!
// Blank page.
//PAGEBREAK!
// Blank page.
//PAGEBREAK!
// Blank page.

