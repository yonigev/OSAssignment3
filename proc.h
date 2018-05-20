// Per-CPU state
struct cpu {
    uchar apicid;                // Local APIC ID
    struct context *scheduler;   // swtch() here to enter scheduler
    struct taskstate ts;         // Used by x86 to find stack for interrupt
    struct segdesc gdt[NSEGS];   // x86 global descriptor table
    volatile uint started;       // Has the CPU started?
    int ncli;                    // Depth of pushcli nesting.
    int intena;                  // Were interrupts enabled before pushcli?
    struct proc *proc;           // The process running on this cpu or null
};

extern struct cpu cpus[NCPU];
extern int ncpu;

//PAGEBREAK: 17
// Saved registers for kernel context switches.
// Don't need to save all the segment registers (%cs, etc),
// because they are constant across kernel contexts.
// Don't need to save %eax, %ecx, %edx, because the
// x86 convention is that the caller has saved them.
// Contexts are stored at the bottom of the stack they
// describe; the stack pointer is the address of the context.
// The layout of the context matches the layout of the stack in swtch.S
// at the "Switch stacks" comment. Switch doesn't save eip explicitly,
// but it is on the stack and allocproc() manipulates it.
struct context {
    uint edi;
    uint esi;
    uint ebx;
    uint ebp;
    uint eip;
};

enum procstate {
    UNUSED, EMBRYO, SLEEPING, RUNNABLE, RUNNING, ZOMBIE
};




struct page{
    int         exists;         //1 -if this page exists, 0 if it's the end of the list.
    void*       vaddr;          //the page's virtual address
    int         in_back;        //1 if in back, 0 if stored in the memory.
    uint        offset;         //offset in Back file ( if in_back == 1 )
    uint        age;            //for NFUA
    uint        age2;           //for LAPA
};
struct p_meta {
    
    struct page         pages[MAX_TOTAL_PAGES];               //    contains virtual addresses. the i'th address means the i'th page
    struct page_queue   pq;                                   //    used for SCFIFO
    int                 offsets[MAX_TOTAL_PAGES];             //    0 if offset #i is available, 1 otherwise (taken by some page)

};



//a page queue .
struct page_queue {
    struct page         pages[MAX_TOTAL_PAGES];
    int lastIndex;
};
// Per-process state
struct proc {
    uint sz;                     // Size of process memory (bytes)
    pde_t *pgdir;                // Page table (Directory) (4KB directory, contains the page addresses, and flags of the SECOND level tables)
    char *kstack;                // Bottom of kernel stack for this process
    enum procstate state;        // Process state
    int pid;                     // Process ID
    struct proc *parent;         // Parent process
    struct trapframe *tf;        // Trap frame for current syscall
    struct context *context;     // swtch() here to run process
    void *chan;                  // If non-zero, sleeping on chan
    int killed;                  // If non-zero, have been killed
    struct file *ofile[NOFILE];  // Open files
    struct inode *cwd;           // Current directory
    char name[16];               // Process name (debugging)
    struct p_meta* paging_meta;
    //Swap file. must initiate with create swap file
    struct file *swapFile;      //page file
};





// Process memory is laid out contiguously, low addresses first:
//   text
//   original data and bss
//   fixed-size stack
//   expandable heap
