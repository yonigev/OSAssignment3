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



//adds a page to the meta data of the Process
int addPageMeta(struct proc* p,int virtual_address,uint file_offset){
    int i;
    for(i=0; i<MAX_TOTAL_PAGES; i++){
        if(p->paging_meta->virtual_address[i] != 0)    //if there's a free spot, take it
            continue;
        p->paging_meta->virtual_address[i]=virtual_address;
        p->paging_meta->num_in_file ++;
        p->paging_meta->offset_swapfile[i]=file_offset;
        p->paging_meta->next_free_offset = file_offset + PGSIZE;        //set the next free file offset
        return 1;
    }
    return 0; //error, no place found
}
//removes a page from the meta data of the Process
int removePageMeta(struct proc* p,int virtual_address,uint file_offset){
    int i;
    for(i=0; i<MAX_TOTAL_PAGES; i++){
        //if the searched VAddress is found with the correct file offset
        if( p->paging_meta->virtual_address[i] == virtual_address   &&p->paging_meta->offset_swapfile[i] == file_offset){
            p->paging_meta->virtual_address[i] = 0;                 //clear the spot
            p->paging_meta->offset_swapfile[i] = 0;                 //clear the offset (not necessary?)
            p->paging_meta->num_in_file --;                         //decrease the counter
            p->paging_meta->next_free_offset = file_offset - PGSIZE;        //set the next free file offset (a page was removed)
            return 1;
        }
    }
    return 0; //error, address not found
}


struct p_meta {
    int  num_in_mem;
    int  num_in_file;
    char *virtual_address[MAX_PSYC_PAGES];     //  contains virtual addresses. the i'th address means the i'th page
    uint offset_swapfile[MAX_PSYC_PAGES];      //  contains the offset of the i'th page in the swapfile
    uint next_free_offset;


};
// Per-process state
struct proc {
    uint sz;                     // Size of process memory (bytes)
    pde_t *pgdir;                // Page table (Directory!?) (4KB directory, contains the page addresses, and flags of the SECOND level tables)
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
