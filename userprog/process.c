#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/syscall.h"



struct process_load_info {
  char *file_name;            // executable file name
  struct semaphore load_sema; 
  bool success;               // flag of load success
  tid_t child_tid;            // child thread's tid
  struct child *child;
};



static struct lock child_lock;
static thread_func start_process NO_RETURN;
static thread_func start_pthread NO_RETURN;
static bool load(const char* file_name, void (**eip)(void), void** esp);
bool setup_thread(void (**eip)(void), void** esp);

/* Initializes user programs in the system by ensuring the main
   thread has a minimal PCB so that it can execute and wait for
   the first user process. Any additions to the PCB should be also
   initialized here if main needs those members */
void userprog_init(void) {
  struct thread* t = thread_current();
  bool success;

  /* Allocate process control block
     It is imoprtant that this is a call to calloc and not malloc,
     so that t->pcb->pagedir is guaranteed to be NULL (the kernel's
     page directory) when t->pcb is assigned, because a timer interrupt
     can come at any time and activate our pagedir */
  t->pcb = calloc(sizeof(struct process), 1);
  success = t->pcb != NULL;
  /* Kill the kernel if we did not succeed */
  ASSERT(success);

  list_init(&t->pcb->children);
  t->pcb->my_info_as_child = NULL;
  lock_init(&child_lock);
  for (int i = 0; i < MAX_FILES; i++) {
    t->pcb->files[i] = NULL;
  }
  t->pcb->next_fd = 2; // 0, 1 for std in and out  
  t->pcb->exec_file = NULL;
}

/* Starts a new thread running a user program loaded from
   FILENAME.  The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   process id, or TID_ERROR if the thread cannot be created. */
pid_t process_execute(const char* file_name) {
  struct process_load_info* info = malloc(sizeof(struct process_load_info));
  if (!info)
  {
    return TID_ERROR;
  }
  
  tid_t tid;
  

  /* Make a copy of FILE_NAME.
     Otherwise there's a race between the caller and load(). */
  info->file_name = palloc_get_page(0);
  if (info->file_name == NULL) {
    free(info);
    return TID_ERROR;
  }
  strlcpy(info->file_name, file_name, PGSIZE);

  sema_init(&info->load_sema,0);
  info->success = false;
  info->child_tid = TID_ERROR;



  info->child = malloc(sizeof(struct child));
  if (info->child == NULL) {
      palloc_free_page(info->file_name);
      free(info);
      return TID_ERROR;
  }


  sema_init(&info->child->wait_sema, 0);
  info->child->waited = false;
  info->child->exited = false;
  info->child->child_thread = NULL;


  /* Create a new thread to execute FILE_NAME. */

  
  tid = thread_create(file_name, PRI_DEFAULT, start_process,(void*) info);
  if (tid == TID_ERROR)
  {
    free(info->child);
    palloc_free_page(info->file_name);
    free(info);
    return tid;
  }

  sema_down(&info->load_sema);


  pid_t result = info->success ? info->child_tid : TID_ERROR;

  if (result != TID_ERROR && thread_current()->pcb != NULL) {
    // success: create child process record which is hang on the parent process list

    lock_acquire(&child_lock);
    
    info->child->pid = result;   
    list_push_back(&thread_current()->pcb->children, &info->child->elem);

    lock_release(&child_lock);

  } else {
    free(info->child);
  }
  sema_up(&info->load_sema);
  free(info);   
  return result;
}

/* A thread function that loads a user process and starts it
   running. */
static void start_process(void* info_) {
  struct process_load_info *info = (struct process_load_info *)info_;
  char* file_name = (char*)info->file_name;
  struct thread* t = thread_current();
  struct intr_frame if_;
  bool success, pcb_success;

  /* Allocate process control block */
  struct process* new_pcb = malloc(sizeof(struct process));
  success = pcb_success = new_pcb != NULL;

  /* Initialize process control block */
  if (success) {
    // Ensure that timer_interrupt() -> schedule() -> process_activate()
    // does not try to activate our uninitialized pagedir
    new_pcb->pagedir = NULL;
    t->pcb = new_pcb;
    // Continue initializing the PCB as normal
    t->pcb->main_thread = t;
    strlcpy(t->pcb->process_name, t->name, sizeof t->name);

    // add init for child process info
    list_init(&t->pcb->children);
    t->pcb->my_info_as_child = NULL;


    // add init for file description table
    for (int i = 0; i < MAX_FILES; i++) {
      new_pcb->files[i] = NULL;
    }
    new_pcb->next_fd = 2; // 0, 1 for std in and out  
    new_pcb->exec_file = NULL;
  }

  // here start analysis the commandline just the file_name
  char *save_ptr;
  char *token;
  char *argv_ptrs[MAX_ARGS];  
  int argc = 0;

  // 3. use strtok_r to divide string
  token = strtok_r(file_name, " ", &save_ptr);


  /* Initialize interrupt frame and load executable. */
  if (success) {
    memset(&if_, 0, sizeof if_);
    if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
    if_.cs = SEL_UCSEG;
    if_.eflags = FLAG_IF | FLAG_MBS;
    success = load(token, &if_.eip, &if_.esp);
  }

  


  /* Handle failure with succesful PCB malloc. Must free the PCB */
  if (!success && pcb_success) {
    // Avoid race where PCB is freed before t->pcb is set to NULL
    // If this happens, then an unfortuantely timed timer interrupt
    // can try to activate the pagedir, but it is now freed memory
    struct process* pcb_to_free = t->pcb;
    t->pcb = NULL;
    free(pcb_to_free);
  }

  /* Clean up. Exit on failure or jump to userspace */
  if (!success) {
    info->child_tid = TID_ERROR;
    info->success = false;
    sema_up(&info->load_sema);
    palloc_free_page(file_name);
    thread_exit();
  }

  t->pcb->my_info_as_child = info->child; 
  info->child->child_thread = t; 
  info->child_tid = t->tid;
  info->success = true;
  sema_up(&info->load_sema);



  // take all token from file_name or call it command_line    
  
  while (token != NULL && argc < MAX_ARGS - 1) {
      argv_ptrs[argc++] = token;
      token = strtok_r(NULL, " ", &save_ptr);
  }
  argv_ptrs[argc] = NULL; // sentinel

  size_t total_size = 0;
  // you should count  the length of string because they all should be put on the stack
  for (int i = 0; i < argc; i++) {
      total_size += strlen(argv_ptrs[i]) + 1;
  }
  total_size += (argc + 1) * sizeof(char *); // include the sentinel so +1 for argc
  total_size += sizeof(char **); // the address of argv array
  total_size += sizeof(int); // argc
  total_size += sizeof(void *); // faked return address

  // uint8_t * equal unsigned char *,it let the + or - only move 1 byte for 1
  uint8_t *user_stack = (uint8_t *) PHYS_BASE;
  user_stack -= total_size;
  user_stack = (uint8_t *)((uint32_t)user_stack & ~0xF); // because bit operation should be used in int instead of pointer so use type change
  // user_stack -= 4;
  uint8_t *cur = user_stack;  
  // faked return address
  *(void **)cur = NULL;
  cur += sizeof(void*);
  // argc
  *(int *)cur = argc;
  cur += sizeof(int);
  // argv[0] is in the low address for argv pointers so here its just + sizeof(char **)
  char **argv_start = (char **)(cur + sizeof(char **));
  *(char ***)cur = argv_start;
  cur += sizeof(char **);

  // just for occupation
  char **argv_array = (char **)cur;
  cur += (argc + 1) * sizeof(char *); // extra 1 for sentinel NULL pointer

  // put the strings into the stack
  char *string_addrs[argc];
  for (int i = 0; i < argc; i++) {
      size_t len = strlen(argv_ptrs[i]) + 1;
      memcpy(cur, argv_ptrs[i], len);
      string_addrs[i] = (char *)cur;
      cur += len;
  }
  // put right pointer into stack
  for (int i = 0; i < argc; i++) {
    argv_array[i] = string_addrs[i];
  }
  argv_array[argc] = NULL; // sentinel
  
  if_.esp = user_stack;

  palloc_free_page(file_name);


  /* Start the user process by simulating a return from an
     interrupt, implemented by intr_exit (in
     threads/intr-stubs.S).  Because intr_exit takes all of its
     arguments on the stack in the form of a `struct intr_frame',
     we just point the stack pointer (%esp) to our stack frame
     and jump to it. */
  asm volatile("movl %0, %%esp; jmp intr_exit" : : "g"(&if_) : "memory");
  NOT_REACHED();
}

/* Waits for process with PID child_pid to die and returns its exit status.
   If it was terminated by the kernel (i.e. killed due to an
   exception), returns -1.  If child_pid is invalid or if it was not a
   child of the calling process, or if process_wait() has already
   been successfully called for the given PID, returns -1
   immediately, without waiting.

   This function will be implemented in problem 2-2.  For now, it
   does nothing. */
int process_wait(pid_t child_pid ) {
  struct thread *cur = thread_current();
  struct child *target = NULL;

  lock_acquire(&child_lock);

  /* 1. seek pid in parent children list */
  struct list *children = &cur->pcb->children;
  for (struct list_elem *e = list_begin(children);
        e != list_end(children); e = list_next(e)) {
      struct child *c = list_entry(e, struct child, elem);
      if (c->pid == child_pid) {
          target = c;
          break;
      }
  }

  /* 2. not direct child process , or have been waited.return -1 */
  if (target == NULL || target->waited) {
      lock_release(&child_lock);
      return -1;
  }

  /* 3. marked waited avoid wait again */
  target->waited = true;

  /* 4. if child process hasn't been exited, release lock and wait child use sema_down */
  if (!target->exited) {
      lock_release(&child_lock);          // release lock let child process can exit
      sema_down(&target->wait_sema);      // block untill child process sema_up
      lock_acquire(&child_lock);          // get lock again to safely change data
  }

  /* 5. get child process exit status */
  int status = target->exit_status;

  /* 6. free child from parent process children list  */
  list_remove(&target->elem);
  free(target);

  lock_release(&child_lock);
  return status;
}


/* Free the current process's resources. */
void process_exit(int status) {
  struct thread* cur = thread_current();
  uint32_t* pd;

  /* If this thread does not have a PCB, don't worry */
  if (cur->pcb == NULL) {
    thread_exit();
    NOT_REACHED();
  }






  lock_acquire(&child_lock);

  struct process *pcb = cur->pcb;
  if ( pcb->my_info_as_child != NULL) {
        struct child *child = pcb->my_info_as_child;
        child->exit_status = status;   // save exit status
        child->exited = true;          // mark exited
        sema_up(&child->wait_sema);    // wake up parent process
  }


  while (!list_empty(&pcb->children)) {
      struct child *c = list_entry(list_pop_front(&pcb->children), struct child, elem);
      if (!c->exited) {
          // set my_info_as_child to NULL
          if (c->child_thread && c->child_thread->pcb) {
              c->child_thread->pcb->my_info_as_child = NULL;
          }
      }
      free(c);
    }


  lock_release(&child_lock);



  lock_acquire(&filesys_lock);

  for (int fd = 2; fd < MAX_FILES; fd++) {
      if (pcb->files[fd] != NULL) {
          file_close(pcb->files[fd]);
          pcb->files[fd] = NULL;
      }
  }

  if (pcb->exec_file != NULL) {
      file_allow_write(pcb->exec_file);
      file_close(pcb->exec_file);
      pcb->exec_file = NULL;
  }  


  lock_release(&filesys_lock);






  /* Destroy the current process's page directory and switch back
     to the kernel-only page directory. */
  pd = cur->pcb->pagedir;
  if (pd != NULL) {
    /* Correct ordering here is crucial.  We must set
         cur->pcb->pagedir to NULL before switching page directories,
         so that a timer interrupt can't switch back to the
         process page directory.  We must activate the base page
         directory before destroying the process's page
         directory, or our active page directory will be one
         that's been freed (and cleared). */
    cur->pcb->pagedir = NULL;
    pagedir_activate(NULL);
    pagedir_destroy(pd);
  }


  /* Free the PCB of this process and kill this thread
     Avoid race where PCB is freed before t->pcb is set to NULL
     If this happens, then an unfortuantely timed timer interrupt
     can try to activate the pagedir, but it is now freed memory */
  struct process* pcb_to_free = cur->pcb;
  cur->pcb = NULL;
  free(pcb_to_free);


  thread_exit();
}

/* Sets up the CPU for running user code in the current
   thread. This function is called on every context switch. */
void process_activate(void) {
  struct thread* t = thread_current();

  /* Activate thread's page tables. */
  if (t->pcb != NULL && t->pcb->pagedir != NULL)
    pagedir_activate(t->pcb->pagedir);
  else
    pagedir_activate(NULL);

  /* Set thread's kernel stack for use in processing interrupts.
     This does nothing if this is not a user process. */
  tss_update();
}



struct fork_aux {
    struct process *pcb;            // pre allocated for child PCB
    struct child *child;            // record for parent child relationship
    struct intr_frame parent_if;    // copy of parent infr_frame
    struct semaphore init_sema; 
};

static void fork_child(void *aux_) {
  if (aux_ == NULL) {
    thread_exit();   // or process_exit(-1)
  }
    struct fork_aux *aux = (struct fork_aux *)aux_;
    struct thread *t = thread_current();

    // set PCB for child thread and main thread for  child process
    t->pcb = aux->pcb;
    t->pcb->main_thread = t;

    // set child and child thread for child process accessing child
    // TODO:here lock is not really necessary, should think about it in the future
    lock_acquire(&child_lock);
    t->pcb->my_info_as_child = aux->child;
    aux->child->child_thread = t;
    lock_release(&child_lock);

    // activate child process page dir
    process_activate();

    sema_up(&aux->init_sema);

    // set intr frame for return to user state,set return value 0
    struct intr_frame if_;
    memcpy(&if_, &aux->parent_if, sizeof if_);
    if_.eax = 0;    // child process return 0

    // free helper struct data which allocated by parent process
    free(aux);

    // goto userspace
    asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g" (&if_) : "memory");
    NOT_REACHED();
}

static bool copy_page_table(uint32_t *dst_pd, uint32_t *src_pd) {
    for (uint32_t vaddr = 0; vaddr < (uint32_t) PHYS_BASE; vaddr += PGSIZE) {
        // get parent process page's kernel virtual address and check if page mapped
        void *src_kpage = pagedir_get_page(src_pd, (void *)vaddr);
        if (src_kpage == NULL)
            continue;   // unmapped,continue

        // get parent mapped page writeable 
        bool writable = pagedir_is_writable(src_pd, (void *)vaddr);

        // allocate new page for child process, return address is kernel virtual address
        void *dst_kpage = palloc_get_page(PAL_USER | PAL_ZERO);
        if (dst_kpage == NULL)
            return false;

        // copy content form parent process page
        memcpy(dst_kpage, src_kpage, PGSIZE);

        // set new allocated page in child process page table
        if (!pagedir_set_page(dst_pd, (void *)vaddr, dst_kpage, writable)) {
            palloc_free_page(dst_kpage);
            return false;
        }
    }
    return true;
}


pid_t process_fork(struct intr_frame *parent_if) {
   struct thread *cur = thread_current();
    struct process *parent_pcb = cur->pcb;

    // 1. create child process page directory
    uint32_t *child_pagedir = pagedir_create();
    if (child_pagedir == NULL)
        return TID_ERROR;

    // 2. copy parent process pages to child process page and set child process page table
    if (!copy_page_table(child_pagedir, parent_pcb->pagedir)) {
        pagedir_destroy(child_pagedir);
        return TID_ERROR;
    }

    // 3. allocate and init child process
    struct process *child_pcb = malloc(sizeof *child_pcb);
    if (child_pcb == NULL) {
        pagedir_destroy(child_pagedir);
        return TID_ERROR;
    }

    child_pcb->pagedir = child_pagedir;
    memcpy(child_pcb->process_name, parent_pcb->process_name,
          sizeof parent_pcb->process_name);
    list_init(&child_pcb->children);
    child_pcb->my_info_as_child = NULL;  
    child_pcb->main_thread = NULL;       
    for (int i = 0; i < MAX_FILES; i++) {
        child_pcb->files[i] = parent_pcb->files[i];
    }
    child_pcb->next_fd = parent_pcb->next_fd;
    
    if (parent_pcb->exec_file != NULL) {
      child_pcb->exec_file = file_reopen(parent_pcb->exec_file);
      if (child_pcb->exec_file != NULL) {
          file_deny_write(child_pcb->exec_file);
      }
    } else {
        child_pcb->exec_file = NULL;
    }



    // 4. construct parent child relationship
    struct child *child = malloc(sizeof *child);
    if (child == NULL) {
        free(child_pcb);
        pagedir_destroy(child_pagedir);
        return TID_ERROR;
    }
    sema_init(&child->wait_sema, 0);
    child->pid = -1;          // temporary unknow
    child->exit_status = -1;
    child->waited = false;
    child->exited = false;
    child->child_thread = NULL; // set bt child process

    // 5. set child process helper function
    struct fork_aux *aux = malloc(sizeof *aux);
    if (aux == NULL) {
        free(child);
        free(child_pcb);
        pagedir_destroy(child_pagedir);
        return TID_ERROR;
    }
    aux->pcb = child_pcb;
    aux->child = child;
    memcpy(&aux->parent_if, parent_if, sizeof *parent_if);
    sema_init(&aux->init_sema, 0);  

    // 6. create child thread
    tid_t child_tid = thread_create(parent_pcb->process_name,
                                    PRI_DEFAULT, fork_child, aux);
    if (child_tid == TID_ERROR) {
        free(aux);
        free(child);
        free(child_pcb);
        pagedir_destroy(child_pagedir);
        return TID_ERROR;
    }
    sema_down(&aux->init_sema); 


    // 7. complete child and insert child to parent process children list to construct parent child relationship
    lock_acquire(&child_lock);
    child->pid = child_tid;
    list_push_back(&parent_pcb->children, &child->elem);
    lock_release(&child_lock);

    // child process my_info_as_child has been set on  fork_child , not again here
    

    return child_tid;
}





































/* We load ELF binaries.  The following definitions are taken
   from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/* For use with ELF types in printf(). */
#define PE32Wx PRIx32 /* Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32 /* Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32 /* Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16 /* Print Elf32_Half in hexadecimal. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
   This appears at the very beginning of an ELF binary. */
struct Elf32_Ehdr {
  unsigned char e_ident[16];
  Elf32_Half e_type;
  Elf32_Half e_machine;
  Elf32_Word e_version;
  Elf32_Addr e_entry;
  Elf32_Off e_phoff;
  Elf32_Off e_shoff;
  Elf32_Word e_flags;
  Elf32_Half e_ehsize;
  Elf32_Half e_phentsize;
  Elf32_Half e_phnum;
  Elf32_Half e_shentsize;
  Elf32_Half e_shnum;
  Elf32_Half e_shstrndx;
};

/* Program header.  See [ELF1] 2-2 to 2-4.
   There are e_phnum of these, starting at file offset e_phoff
   (see [ELF1] 1-6). */
struct Elf32_Phdr {
  Elf32_Word p_type;
  Elf32_Off p_offset;
  Elf32_Addr p_vaddr;
  Elf32_Addr p_paddr;
  Elf32_Word p_filesz;
  Elf32_Word p_memsz;
  Elf32_Word p_flags;
  Elf32_Word p_align;
};

/* Values for p_type.  See [ELF1] 2-3. */
#define PT_NULL 0           /* Ignore. */
#define PT_LOAD 1           /* Loadable segment. */
#define PT_DYNAMIC 2        /* Dynamic linking info. */
#define PT_INTERP 3         /* Name of dynamic loader. */
#define PT_NOTE 4           /* Auxiliary info. */
#define PT_SHLIB 5          /* Reserved. */
#define PT_PHDR 6           /* Program header table. */
#define PT_STACK 0x6474e551 /* Stack segment. */

/* Flags for p_flags.  See [ELF3] 2-3 and 2-4. */
#define PF_X 1 /* Executable. */
#define PF_W 2 /* Writable. */
#define PF_R 4 /* Readable. */

static bool setup_stack(void** esp);
static bool validate_segment(const struct Elf32_Phdr*, struct file*);
static bool load_segment(struct file* file, off_t ofs, uint8_t* upage, uint32_t read_bytes,
                         uint32_t zero_bytes, bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
   Stores the executable's entry point into *EIP
   and its initial stack pointer into *ESP.
   Returns true if successful, false otherwise. */
bool load(const char* file_name, void (**eip)(void), void** esp) {
  struct thread* t = thread_current();
  struct Elf32_Ehdr ehdr;
  struct file* file = NULL;
  off_t file_ofs;
  bool success = false;
  int i;

  /* Allocate and activate page directory. */
  t->pcb->pagedir = pagedir_create();
  if (t->pcb->pagedir == NULL)
    goto done;
  process_activate();

  /* Open executable file. */
  file = filesys_open(file_name);
  if (file == NULL) {
    printf("load: %s: open failed\n", file_name);
    goto done;
  }

  /* Read and verify executable header. */
  if (file_read(file, &ehdr, sizeof ehdr) != sizeof ehdr ||
      memcmp(ehdr.e_ident, "\177ELF\1\1\1", 7) || ehdr.e_type != 2 || ehdr.e_machine != 3 ||
      ehdr.e_version != 1 || ehdr.e_phentsize != sizeof(struct Elf32_Phdr) || ehdr.e_phnum > 1024) {
    printf("load: %s: error loading executable\n", file_name);
    goto done;
  }

  /* Read program headers. */
  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++) {
    struct Elf32_Phdr phdr;

    if (file_ofs < 0 || file_ofs > file_length(file))
      goto done;
    file_seek(file, file_ofs);

    if (file_read(file, &phdr, sizeof phdr) != sizeof phdr)
      goto done;
    file_ofs += sizeof phdr;
    switch (phdr.p_type) {
      case PT_NULL:
      case PT_NOTE:
      case PT_PHDR:
      case PT_STACK:
      default:
        /* Ignore this segment. */
        break;
      case PT_DYNAMIC:
      case PT_INTERP:
      case PT_SHLIB:
        goto done;
      case PT_LOAD:
        if (validate_segment(&phdr, file)) {
          bool writable = (phdr.p_flags & PF_W) != 0;
          uint32_t file_page = phdr.p_offset & ~PGMASK;
          uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
          uint32_t page_offset = phdr.p_vaddr & PGMASK;
          uint32_t read_bytes, zero_bytes;
          if (phdr.p_filesz > 0) {
            /* Normal segment.
                     Read initial part from disk and zero the rest. */
            read_bytes = page_offset + phdr.p_filesz;
            zero_bytes = (ROUND_UP(page_offset + phdr.p_memsz, PGSIZE) - read_bytes);
          } else {
            /* Entirely zero.
                     Don't read anything from disk. */
            read_bytes = 0;
            zero_bytes = ROUND_UP(page_offset + phdr.p_memsz, PGSIZE);
          }
          if (!load_segment(file, file_page, (void*)mem_page, read_bytes, zero_bytes, writable))
            goto done;
        } else
          goto done;
        break;
    }
  }

  /* Set up stack. */
  if (!setup_stack(esp))
    goto done;

  /* Start address. */
  *eip = (void (*)(void))ehdr.e_entry;

  success = true;
  if (success)
  {
    file_deny_write(file);
    t->pcb->exec_file = file;
  }
  


done:
  /* We arrive here whether the load is successful or not. */
  if (!success && file != NULL )
  {
    file_close(file);
  }
  return success;
}

/* load() helpers. */

static bool install_page(void* upage, void* kpage, bool writable);

/* Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
static bool validate_segment(const struct Elf32_Phdr* phdr, struct file* file) {
  /* p_offset and p_vaddr must have the same page offset. */
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
    return false;

  /* p_offset must point within FILE. */
  if (phdr->p_offset > (Elf32_Off)file_length(file))
    return false;

  /* p_memsz must be at least as big as p_filesz. */
  if (phdr->p_memsz < phdr->p_filesz)
    return false;

  /* The segment must not be empty. */
  if (phdr->p_memsz == 0)
    return false;

  /* The virtual memory region must both start and end within the
     user address space range. */
  if (!is_user_vaddr((void*)phdr->p_vaddr))
    return false;
  if (!is_user_vaddr((void*)(phdr->p_vaddr + phdr->p_memsz)))
    return false;

  /* The region cannot "wrap around" across the kernel virtual
     address space. */
  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
    return false;

  /* Disallow mapping page 0.
     Not only is it a bad idea to map page 0, but if we allowed
     it then user code that passed a null pointer to system calls
     could quite likely panic the kernel by way of null pointer
     assertions in memcpy(), etc. */
  if (phdr->p_vaddr < PGSIZE)
    return false;

  /* It's okay. */
  return true;
}

/* Loads a segment starting at offset OFS in FILE at address
   UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
   memory are initialized, as follows:

        - READ_BYTES bytes at UPAGE must be read from FILE
          starting at offset OFS.

        - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.

   The pages initialized by this function must be writable by the
   user process if WRITABLE is true, read-only otherwise.

   Return true if successful, false if a memory allocation error
   or disk read error occurs. */
static bool load_segment(struct file* file, off_t ofs, uint8_t* upage, uint32_t read_bytes,
                         uint32_t zero_bytes, bool writable) {
  ASSERT((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT(pg_ofs(upage) == 0);
  ASSERT(ofs % PGSIZE == 0);

  file_seek(file, ofs);
  while (read_bytes > 0 || zero_bytes > 0) {
    /* Calculate how to fill this page.
         We will read PAGE_READ_BYTES bytes from FILE
         and zero the final PAGE_ZERO_BYTES bytes. */
    size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
    size_t page_zero_bytes = PGSIZE - page_read_bytes;

    /* Get a page of memory. */
    uint8_t* kpage = palloc_get_page(PAL_USER);
    if (kpage == NULL)
      return false;

    /* Load this page. */
    if (file_read(file, kpage, page_read_bytes) != (int)page_read_bytes) {
      palloc_free_page(kpage);
      return false;
    }
    memset(kpage + page_read_bytes, 0, page_zero_bytes);

    /* Add the page to the process's address space. */
    if (!install_page(upage, kpage, writable)) {
      palloc_free_page(kpage);
      return false;
    }

    /* Advance. */
    read_bytes -= page_read_bytes;
    zero_bytes -= page_zero_bytes;
    upage += PGSIZE;
  }
  return true;
}

/* Create a minimal stack by mapping a zeroed page at the top of
   user virtual memory. */
static bool setup_stack(void** esp) {
  uint8_t* kpage;
  bool success = false;

  kpage = palloc_get_page(PAL_USER | PAL_ZERO);
  if (kpage != NULL) {
    success = install_page(((uint8_t*)PHYS_BASE) - PGSIZE, kpage, true);
    if (success)
      *esp = PHYS_BASE;
    else
      palloc_free_page(kpage);
  }

  // TODO: add the argv and the argc and the NULL return address and some NULL sentinel
  // TODO: this function now only put correct value to  the esp  but others are less.

  return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
   virtual address KPAGE to the page table.
   If WRITABLE is true, the user process may modify the page;
   otherwise, it is read-only.
   UPAGE must not already be mapped.
   KPAGE should probably be a page obtained from the user pool
   with palloc_get_page().
   Returns true on success, false if UPAGE is already mapped or
   if memory allocation fails. */
static bool install_page(void* upage, void* kpage, bool writable) {
  struct thread* t = thread_current();

  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
  return (pagedir_get_page(t->pcb->pagedir, upage) == NULL &&
          pagedir_set_page(t->pcb->pagedir, upage, kpage, writable));
}

/* Returns true if t is the main thread of the process p */
bool is_main_thread(struct thread* t, struct process* p) { return p->main_thread == t; }

/* Gets the PID of a process */
pid_t get_pid(struct process* p) { return (pid_t)p->main_thread->tid; }

/* Creates a new stack for the thread and sets up its arguments.
   Stores the thread's entry point into *EIP and its initial stack
   pointer into *ESP. Handles all cleanup if unsuccessful. Returns
   true if successful, false otherwise.

   This function will be implemented in Project 2: Multithreading. For
   now, it does nothing. You may find it necessary to change the
   function signature. */
bool setup_thread(void (**eip)(void) UNUSED, void** esp UNUSED) { return false; }

/* Starts a new thread with a new user stack running SF, which takes
   TF and ARG as arguments on its user stack. This new thread may be
   scheduled (and may even exit) before pthread_execute () returns.
   Returns the new thread's TID or TID_ERROR if the thread cannot
   be created properly.

   This function will be implemented in Project 2: Multithreading and
   should be similar to process_execute (). For now, it does nothing.
   */
tid_t pthread_execute(stub_fun sf UNUSED, pthread_fun tf UNUSED, void* arg UNUSED) { return -1; }

/* A thread function that creates a new user thread and starts it
   running. Responsible for adding itself to the list of threads in
   the PCB.

   This function will be implemented in Project 2: Multithreading and
   should be similar to start_process (). For now, it does nothing. */
static void start_pthread(void* exec_ UNUSED) {}

/* Waits for thread with TID to die, if that thread was spawned
   in the same process and has not been waited on yet. Returns TID on
   success and returns TID_ERROR on failure immediately, without
   waiting.

   This function will be implemented in Project 2: Multithreading. For
   now, it does nothing. */
tid_t pthread_join(tid_t tid UNUSED) { return -1; }

/* Free the current thread's resources. Most resources will
   be freed on thread_exit(), so all we have to do is deallocate the
   thread's userspace stack. Wake any waiters on this thread.

   The main thread should not use this function. See
   pthread_exit_main() below.

   This function will be implemented in Project 2: Multithreading. For
   now, it does nothing. */
void pthread_exit(void) {}

/* Only to be used when the main thread explicitly calls pthread_exit.
   The main thread should wait on all threads in the process to
   terminate properly, before exiting itself. When it exits itself, it
   must terminate the process in addition to all necessary duties in
   pthread_exit.

   This function will be implemented in Project 2: Multithreading. For
   now, it does nothing. */
void pthread_exit_main(void) {}

