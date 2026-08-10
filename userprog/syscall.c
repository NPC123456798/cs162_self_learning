#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/process.h"
#include "userprog/pagedir.h"
#include "devices/shutdown.h"
#include "filesys/filesys.h"
#include "lib/kernel/console.h"
#include "devices/input.h"

#define MAX_USER_LOCKS 256

struct user_lock_entry {
    struct lock kernel_lock;
    bool in_use;
    pid_t owner_pid;
};

static struct user_lock_entry lock_table[MAX_USER_LOCKS];
static struct lock table_lock;      // protect lock_table's locks


struct lock filesys_lock;   // protect file system operation global lock

static bool verify_user_range(const void *uaddr, size_t size);
static bool verify_user_string(const char *str);
static int get_user (const uint8_t *uaddr);
static bool put_user (uint8_t *udst, uint8_t byte);

static void sys_exit(struct intr_frame* f);
static void sys_practice(struct intr_frame* f, uint32_t* args);
static void sys_exec(struct intr_frame* f, uint32_t* args);
static void sys_wait(struct intr_frame* f, uint32_t* args);
static void sys_create(struct intr_frame* f, uint32_t* args);
static void sys_remove(struct intr_frame* f, uint32_t* args);
static void sys_open(struct intr_frame* f, uint32_t* args);
static void sys_filesize(struct intr_frame* f, uint32_t* args);
static void sys_read(struct intr_frame* f, uint32_t* args);
static void sys_write(struct intr_frame* f, uint32_t* args);
static void sys_seek(struct intr_frame* f, uint32_t* args);
static void sys_tell(struct intr_frame* f, uint32_t* args);
static void sys_close(struct intr_frame* f, uint32_t* args);
static void sys_pt_create(struct intr_frame* f, uint32_t* args);
static void sys_pt_exit(struct intr_frame* f, uint32_t* args);
static void sys_pt_join(struct intr_frame* f, uint32_t* args);
static void sys_lock_init(struct intr_frame* f, uint32_t* args);
static void sys_lock_acquire(struct intr_frame* f, uint32_t* args);
static void sys_lock_release(struct intr_frame* f, uint32_t* args);
static void sys_sema_init(struct intr_frame* f, uint32_t* args);
static void sys_sema_down(struct intr_frame* f, uint32_t* args);
static void sys_sema_up(struct intr_frame* f, uint32_t* args);
static void sys_get_tid(struct intr_frame* f, uint32_t* args);


static void syscall_handler(struct intr_frame*);

void syscall_init(void) { 
    intr_register_int(0x30, 3, INTR_ON, syscall_handler, "syscall");
    lock_init(&filesys_lock);
    lock_init(&table_lock);
}

static void syscall_handler(struct intr_frame* f ) {
  if (f->esp == NULL || !is_user_vaddr((void*)f->esp)) {
    process_exit(-1);
    return;
  }
  uint32_t* args = ((uint32_t*)f->esp);
  if (!verify_user_range(args,sizeof(int))) {
    process_exit(-1);
    return;
  }
  uint32_t syscall_no = args[0];




  switch (syscall_no)
  {
    case SYS_HALT:
        {
            shutdown_power_off();
            break;
        }
    case SYS_EXIT:
        sys_exit(f);
        break;
        
    case SYS_PRACTICE:
        sys_practice(f, args);
        break;
        
    case SYS_EXEC:
        sys_exec(f,args);
        break;

        
    case SYS_WAIT:
        sys_wait(f,args);
        break;
    
    case SYS_FORK:
        {     
            f->eax = process_fork(f);
            break;
        }

    case SYS_CREATE:
        sys_create(f, args);
        break;
        

    case SYS_REMOVE:
        sys_remove(f, args);
        break;


    case SYS_OPEN:
        sys_open(f, args);
        break;

    case SYS_FILESIZE:
        sys_filesize(f, args);
        break;


    case SYS_READ:
        sys_read(f, args);
        break;

    case SYS_WRITE:
        sys_write(f,args);
        break;    

    case SYS_SEEK:
        sys_seek(f, args);
        break;

    case SYS_TELL:
        sys_tell(f,args);
        break;


    case SYS_CLOSE:
        sys_close(f, args);
        break;

    case SYS_PT_CREATE:
        sys_pt_create(f,args);
        break;

    case SYS_PT_EXIT:
        sys_pt_exit(f,args);
        break;

    case SYS_PT_JOIN:
        sys_pt_join(f,args);
        break;


    case SYS_LOCK_INIT:
        sys_lock_init(f,args);
        break;



    case SYS_LOCK_ACQUIRE:
        sys_lock_acquire(f,args);
        break;




    case SYS_LOCK_RELEASE:
        sys_lock_release(f,args);
        break;




    case SYS_SEMA_INIT:
        sys_sema_init(f,args);
        break;



    case SYS_SEMA_DOWN:
        sys_sema_down(f,args);
        break;



    case SYS_SEMA_UP:
        sys_sema_up(f,args);
        break;




    case SYS_GET_TID:
        sys_get_tid(f,args);
        break;





    default:

        process_exit(-1);
        break;
    }










  /*
   * The following print statement, if uncommented, will print out the syscall
   * number whenever a process enters a system call. You might find it useful
   * when debugging. It will cause tests to fail, however, so you should not
   * include it in your final submission.
   */

  /* printf("System call number: %d\n", args[0]); */


}



/* Reads a byte at user virtual address UADDR.
   UADDR must be below PHYS_BASE.
   Returns the byte value if successful,
   -1 if a segfault occurred. */
static int get_user (const uint8_t *uaddr) {
    int result;
    asm ("movl $1f, %0; movzbl %1, %0; 1:"
    : "=&a" (result) : "m" (*uaddr));
    return result;
}

/* Writes BYTE to user address UDST.
   UDST must be below PHYS_BASE.
   Returns true if successful,
   false if a segfault occurred. */
static bool put_user (uint8_t *udst, uint8_t byte) {
    int error_code;
    asm ("movl $1f, %0; movb %b2, %1; 1:"
    : "=&a" (error_code), "=m" (*udst) : "q" (byte));
    return error_code != -1;
}









/* check uaddr valid and check the size of bytes pointed by uaddr if every byte valid  */
static bool verify_user_range(const void *uaddr, size_t size) {
    if (size == 0)
    {
        return true;
    }
    
    if (!is_user_vaddr(uaddr))
    { 
        return false;
    }

    uint8_t *start = (uint8_t *)uaddr;
    uint8_t *end = start + size;
    // check circle or overflow into kernel.
    if (end < start || end > (uint8_t *)PHYS_BASE){
        return false;
    } 

    uint32_t *pd = thread_current()->pcb->pagedir;

    // check start byte if is in the page
    if (get_user(start) == -1)
        return false;

    // check the end byte if is in the page
    if (get_user(end - 1) == -1)
        return false;

    return true;
}

/* check str pointer valid and bytes it pointing to validation */
static bool verify_user_string(const char *str) {
    if (!is_user_vaddr(str))
        return false;
    const uint8_t *p = (const uint8_t *)str;
    while (p < (uint8_t *)PHYS_BASE) {
        int byte = get_user(p);
        if (byte == -1)
            return false;
        if (byte == '\0')
            return true;
        p++;
    }
    return false;   /* across user address can't find end symbol*/
}




static void sys_practice(struct intr_frame* f, uint32_t* args) {
            if (!verify_user_range((void*)(f->esp + 4), sizeof(int))) {
                process_exit(-1);
            }
            int i = *(int*)(f->esp + 4);
            f->eax = i + 1;
        }

static void sys_exit(struct intr_frame* f) {
    if (!verify_user_range((void*)(f->esp + 4), sizeof(int))) {
        process_exit(-1);
    }
    int status = *(int*)(f->esp + 4);
    f->eax = status;
    

    process_exit(status);

}

static void sys_exec(struct intr_frame* f, uint32_t* args) {    
    if (!verify_user_range((void*)(f->esp + 4), sizeof(char*))) {
        process_exit(-1);
    }
    char* cmd_line = *(char**)(f->esp + 4);

    if (!verify_user_string(cmd_line)) {
        process_exit(-1);
    }
    f->eax = process_execute(cmd_line);
}




static void sys_wait(struct intr_frame* f, uint32_t* args) {
            if (!verify_user_range((void*)(f->esp + 4), sizeof(int))) {
                process_exit(-1);
                return;
            }
            pid_t pid = *(pid_t*)(f->esp + 4);
            f->eax = process_wait(pid);
        }









static void sys_create(struct intr_frame* f, uint32_t* args) {    
    // verify argument file name pointer itself
    if (!verify_user_range(&args[1], sizeof(char*))) {
        process_exit(-1);
    }
    char *file = (char*)args[1];
    // verify file name string 
    if (!verify_user_string(file)) {
        process_exit(-1);
    }
    // verify initial size argument
    if (!verify_user_range(&args[2], sizeof(unsigned))) {
        process_exit(-1);
    }
    unsigned initial_size = (unsigned)args[2];
    
    lock_acquire(&filesys_lock);
    bool ok = filesys_create(file, initial_size);
    lock_release(&filesys_lock);
    f->eax = ok;

}












static void sys_remove(struct intr_frame* f, uint32_t* args) {    
    // verify argument file name pointer itself
    if (!verify_user_range(&args[1], sizeof(char *))) {
        process_exit(-1);
    }
    char *file = *(char **)&args[1];
    // verify file name string 
    if (!verify_user_string(file)) {
        process_exit(-1);
    }

    lock_acquire(&filesys_lock);
    bool ok = filesys_remove(file);
    lock_release(&filesys_lock);

    f->eax = ok;

}



static void sys_open(struct intr_frame* f, uint32_t* args) {    
    // verify argument file name pointer itself
    if (!verify_user_range(&args[1], sizeof(char *))) {
        process_exit(-1);
    }
    char *file = *(char **)&args[1];
    // verify file name string 
    if (!verify_user_string(file)) {
        process_exit(-1);
    }

    lock_acquire(&filesys_lock);
    struct file *f_ptr = filesys_open(file);
    if (f_ptr == NULL) {
        lock_release(&filesys_lock);
        f->eax = -1;
        return;
    }

    // allocate file description and jump over 0 and 1
    struct thread *cur = thread_current();
    struct process *pcb = cur->pcb;
    int fd = -1;
    for (fd = 2; fd < MAX_FILES; fd++) {
        if (pcb->files[fd] == NULL) {
            pcb->files[fd] = f_ptr;
            break;
        }
    }

    if (fd == MAX_FILES) {
        // if file description table is full or NULL not find then return -1
        file_close(f_ptr);
        f->eax = -1;
    } else {
        f->eax = fd;
    }
    lock_release(&filesys_lock);


}



static void sys_filesize(struct intr_frame* f, uint32_t* args){    
    // verify fd 
    if (!verify_user_range(&args[1], sizeof(int))) {
        process_exit(-1);
    }
    int fd = args[1];
    struct thread *cur = thread_current();
    struct process *pcb = cur->pcb;
    
    // check fd validation 
    if (fd < 0 || fd >= MAX_FILES || pcb->files[fd] == NULL) {
        f->eax = -1;
        return;
    }
    
    lock_acquire(&filesys_lock);
    off_t file_size = file_length(pcb->files[fd]);
    lock_release(&filesys_lock);
    f->eax = file_size;
}




static void sys_read(struct intr_frame* f, uint32_t* args){    
    // verify fd
    if (!verify_user_range(&args[1], sizeof(int))) {
        process_exit(-1);
    }
    int fd = args[1];
    // verify buffer pointer
    if (!verify_user_range(&args[2], sizeof(void *))) {
        process_exit(-1);
    }
    void *buffer = *(void **)&args[2];
    // verify size
    if (!verify_user_range(&args[3], sizeof(unsigned))) {
        process_exit(-1);
    }
    unsigned read_size = args[3];
    // verify buffer writable (whole block)
    if (!verify_user_range(buffer, read_size)) {
        process_exit(-1);
    }



    if (read_size > 0) {
        uint8_t *buf_start = (uint8_t *) buffer;
        uint8_t *buf_end   = buf_start + read_size - 1;
        uint32_t *pd = thread_current()->pcb->pagedir;
        for (uint8_t *page = pg_round_down(buf_start); page <= buf_end; page += PGSIZE) {
            if (!pagedir_is_writable(pd, page)) {
                process_exit(-1);
                return;  
            }
        }
    }

    struct thread *cur = thread_current();
    struct process *pcb = cur->pcb;
    
    if (fd == STDIN_FILENO) {  // 0
        // stdin: input with byte one byte
        int total = 0;
        uint8_t *buf = (uint8_t *)buffer;
        for (unsigned i = 0; i < read_size; i++) {
            int c = input_getc();
            if (c == -1)
                break;
            buf[i] = (uint8_t)c;
            total++;
        }
        f->eax = total;
    } else if (fd < 0 || fd >= MAX_FILES || pcb->files[fd] == NULL) {
        f->eax = -1;
    } else {
        lock_acquire(&filesys_lock);
        // record change for sample.txt
        off_t bytes = file_read(pcb->files[fd], buffer, (off_t) read_size);
        lock_release(&filesys_lock);
        f->eax = bytes;
    }
}


static void sys_write(struct intr_frame* f, uint32_t* args){    
    // verify fd
    if (!verify_user_range(&args[1], sizeof(int))) {
        process_exit(-1);
    }
    int fd = args[1];
    // verify buffer pointer
    if (!verify_user_range(&args[2], sizeof(void *))) {
        process_exit(-1);
    }
    const void *buffer = *(const void **)&args[2];
    // verify size
    if (!verify_user_range(&args[3], sizeof(unsigned))) {
        process_exit(-1);
    }
    unsigned write_size = args[3];
    // verify buffer readable (whole block)
    if (!verify_user_range((void *)buffer, write_size)) {
        process_exit(-1);
    }
    
    struct thread *cur = thread_current();
    struct process *pcb = cur->pcb;
    
    if (fd == STDOUT_FILENO) {  // 1
        putbuf((const char *)buffer, write_size);
        f->eax = write_size;
    } else if (fd < 0 || fd >= MAX_FILES || pcb->files[fd] == NULL) {
        f->eax = -1;
    } else {
        lock_acquire(&filesys_lock);
        off_t bytes = file_write(pcb->files[fd], buffer, write_size);
        lock_release(&filesys_lock);
        f->eax = bytes;
    }
}



static void sys_seek(struct intr_frame* f, uint32_t* args) {    
    // verify fd
    if (!verify_user_range(&args[1], sizeof(int))) {
        process_exit(-1);
    }
    int fd = args[1];
    // verify position
    if (!verify_user_range(&args[2], sizeof(unsigned))) {
        process_exit(-1);
    }
    unsigned position = args[2];
    
    struct thread *cur = thread_current();
    struct process *pcb = cur->pcb;
    
    if (fd < 0 || fd >= MAX_FILES || pcb->files[fd] == NULL) {
        // if fd invalid do nothing
        return;
    }
    
    lock_acquire(&filesys_lock);
    file_seek(pcb->files[fd], position);
    lock_release(&filesys_lock);
}



static void sys_tell(struct intr_frame* f, uint32_t* args) {
    // verify fd
    if (!verify_user_range(&args[1], sizeof(int))) {
        process_exit(-1);
    }
    int fd = args[1];
    struct thread *cur = thread_current();
    struct process *pcb = cur->pcb;
    
    if (fd < 0 || fd >= MAX_FILES || pcb->files[fd] == NULL) {
        f->eax = -1;  // just return -1
        return;
    }
    
    lock_acquire(&filesys_lock);
    off_t pos = file_tell(pcb->files[fd]);
    lock_release(&filesys_lock);
    f->eax = pos;
}




static void sys_close(struct intr_frame* f, uint32_t* args) {    
    // verify fd
    if (!verify_user_range(&args[1], sizeof(int))) {
        process_exit(-1);
    }
    int fd = args[1];
    struct thread *cur = thread_current();
    struct process *pcb = cur->pcb;
    
    if (fd < 0 || fd >= MAX_FILES || pcb->files[fd] == NULL) {
        f->eax = -1;  // just return -1 if fd invalid
        return;
    }
    
    lock_acquire(&filesys_lock);
    file_close(pcb->files[fd]);
    pcb->files[fd] = NULL;
    lock_release(&filesys_lock);
}











/*  */
static void sys_pt_create(struct intr_frame* f, uint32_t* args) {

}

/*  */
static void sys_pt_exit(struct intr_frame* f, uint32_t* args) {

}

/*  */
static void sys_pt_join(struct intr_frame* f, uint32_t* args) {

}

/* init lock in kernel and return the kernel lock's idx for user's lock_t
    lock_t is a map for user using lock  */
static void sys_lock_init(struct intr_frame* f, uint32_t* args) {
    /* check lock_t* argument itself validation */
    if (!verify_user_range(&args[1], sizeof(char*))) {
        process_exit(-1);
    }
    char* lock_item = args[1];

    /* check lock the char's validation */
    if (!verify_user_range(lock_item, sizeof(char))) {
        process_exit(-1);
    }

    lock_acquire(&table_lock);
        
    int idx = -1;
    for (int i = 0; i < MAX_USER_LOCKS; i++) {
        if (!lock_table[i].in_use) {
            idx = i;
            break;
        }
    }

    if (idx == -1) {
        lock_release(&table_lock);
        f->eax = false;
        return;
    }

    // init kernel lock
    lock_init(&lock_table[idx].kernel_lock);
    lock_table[idx].in_use = true;
    lock_table[idx].owner_pid = get_pid(thread_current()->pcb);


    lock_release(&table_lock);


    *lock_item = (char)idx;
    f->eax = true;
}

/* try to acquire lock and check if lock has been held by self or held by other process thread 
    or haven't been register in lock_init */
static void sys_lock_acquire(struct intr_frame* f, uint32_t* args) {

    /* check lock_t* argument itself validation */
    if (!verify_user_range(&args[1], sizeof(char*))) {
        process_exit(-1);
    }
    char* lock_item = args[1];

    /* check lock the char's validation */
    if (!verify_user_range(lock_item, sizeof(char))) {
        process_exit(-1);
    }

    int lock_idx = (int)*lock_item;


    /* protect lock table and then check logic  */
    lock_acquire(&table_lock);

    // check 1: ID if valid and registered  
    if (lock_idx < 0 || lock_idx >= MAX_USER_LOCKS ||
        !lock_table[lock_idx].in_use ||
        lock_table[lock_idx].owner_pid != get_pid(thread_current()->pcb)) {
        lock_release(&table_lock);
        f->eax = false;
        return;
    }

    // check 2: current thread if hold this lock 
    struct lock *k_lock = &lock_table[lock_idx].kernel_lock;
    if (k_lock->holder == thread_current()) {
        lock_release(&table_lock);
        f->eax = false;
        return;
    }

    lock_release(&table_lock);

    /* safely get lock maybe blocked  */
    lock_acquire(k_lock);
    f->eax = true;
}

/*  */
static void sys_lock_release(struct intr_frame* f, uint32_t* args) {

}

/*  */
static void sys_sema_init(struct intr_frame* f, uint32_t* args) {

}

/*  */
static void sys_sema_down(struct intr_frame* f, uint32_t* args) {

}

/*  */
static void sys_sema_up(struct intr_frame* f, uint32_t* args) {

}

/*  */
static void sys_get_tid(struct intr_frame* f, uint32_t* args) {

}






