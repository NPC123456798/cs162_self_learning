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
#define MAX_USER_SEMAS 256

struct user_lock_entry {
    struct lock kernel_lock;
    bool in_use;
    pid_t owner_pid;
};

static struct user_lock_entry lock_table[MAX_USER_LOCKS];
static struct lock table_lock;      // protect lock_table's locks

struct user_sema_entry {
    struct semaphore kernel_sema;
    bool in_use;
    pid_t owner_pid;
};

static struct user_sema_entry sema_table[MAX_USER_SEMAS];

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



/* args for function arguments is from left to right, it means args[1] if leftest argument */
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
    

    bool ok = filesys_create(file, initial_size);

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


    bool ok = filesys_remove(file);


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


    struct file *f_ptr = filesys_open(file);
    if (f_ptr == NULL) {

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
    
    off_t file_size = file_length(pcb->files[fd]);

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

        // record change for sample.txt
        off_t bytes = file_read(pcb->files[fd], buffer, (off_t) read_size);

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

        off_t bytes = file_write(pcb->files[fd], buffer, write_size);

        f->eax = bytes;
    }
}



static void sys_seek(struct intr_frame* f UNUSED, uint32_t* args) {    
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
    

    file_seek(pcb->files[fd], position);

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
    

    off_t pos = file_tell(pcb->files[fd]);

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
    

    file_close(pcb->files[fd]);
    pcb->files[fd] = NULL;

}











/* create user thread for user multi thread */
static void sys_pt_create(struct intr_frame* f, uint32_t* args) {
 /* 验证参数数组 args 中 args[1]、args[2]、args[3] 本身是否可读 */
    if (!verify_user_range(&args[1], sizeof(stub_fun)) ||
        !verify_user_range(&args[2], sizeof(pthread_fun)) ||
        !verify_user_range(&args[3], sizeof(void*))) {
        process_exit(-1);
    }

    stub_fun sfun = (stub_fun) args[1];
    pthread_fun tfun = (pthread_fun) args[2];
    void* arg = (void*) args[3];

    /* verufy address pointedd by sfun and tfun if readable   */
    if (!verify_user_range(sfun, sizeof(stub_fun)) ||
        !verify_user_range(tfun, sizeof(pthread_fun))) {
        f->eax = TID_ERROR;
        return;
    }

    /* arg can be if not NULL then verify its pointing memory readable   */
    if (arg != NULL && !verify_user_range(arg, 1)) {
        f->eax = TID_ERROR;
        return;
    }

    /* call pthread_execute to create user thread   */
    tid_t tid = pthread_execute(sfun, tfun, arg);
    f->eax = tid;
}

/* use pthread_exit, for main thread exit use special pthread_exit_main */
static void sys_pt_exit(struct intr_frame* f, uint32_t* args UNUSED) {
        struct thread *cur = thread_current();
    struct process *pcb = cur->pcb;

    // current thread must belongs to a user process 
    if (pcb == NULL) {
        // kernel thread shouldn't use this syscall, destroy it
        thread_exit();
    }

    if (cur == pcb->main_thread) {
        pthread_exit_main();   // main thread: wait all child threads then exit 
    } else {
        pthread_exit();        // normal thread free source and exit 
    }
}

/*  call kernel's pthread_join */
static void sys_pt_join(struct intr_frame* f, uint32_t* args) {
     // argument check, make sure args[1] is valid in user stack store position    
    if (!verify_user_range(&args[1], sizeof(tid_t))) {
        f->eax = TID_ERROR;
        return;
    }


    tid_t tid = (tid_t) args[1];

    // call kernel's  pthread_join 
    f->eax = pthread_join(tid);

}

/* init lock in kernel and return the kernel lock's idx for user's lock_t
    lock_t is a map for user using lock  */
static void sys_lock_init(struct intr_frame* f, uint32_t* args) {
    /* check lock_t* argument itself validation */
    if (!verify_user_range(&args[1], sizeof(char*))) {
        process_exit(-1);
    }
    char* lock_item = args[1];
    if (lock_item == NULL)
    {
        f->eax = false;
        return;
    }
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
    if (lock_item == NULL)
    {
        f->eax = false;
        return;
    }
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

/* release lock and check the lock's holder and the lock_t argument if valid and it mapped lock if valid
    as lock's attribution */
static void sys_lock_release(struct intr_frame* f, uint32_t* args) {
    /* check lock_t* argument itself validation */
    if (!verify_user_range(&args[1], sizeof(char*))) {
        process_exit(-1);
    }
    char* lock_item = args[1];
    if (lock_item == NULL)
    {
        f->eax = false;
        return;
    }
    /* check lock the char's validation */
    if (!verify_user_range(lock_item, sizeof(char))) {
        process_exit(-1);
    }

    int lock_idx = (int)*lock_item;

    lock_acquire(&table_lock);

    // check 1: ID if valid and registered  
    if (lock_idx < 0 || lock_idx >= MAX_USER_LOCKS ||
        !lock_table[lock_idx].in_use ||
        lock_table[lock_idx].owner_pid != get_pid(thread_current()->pcb)) {
        lock_release(&table_lock);
        f->eax = false;
        return;
    }

    // get kernel lock pointer 
    struct lock* k_lock = &lock_table[lock_idx].kernel_lock;

    // check 2: current thread if hold this lock 
    // on;y holder has right to release 
    if (k_lock->holder != thread_current()) {
        lock_release(&table_lock);
        f->eax = false;
        return;
    }


    // must release table lock before lock_release avoid nested hold lock 
    lock_release(&table_lock);


    
    // call lock_release to release lock and maybe invoke thread which wait this lock 
    lock_release(k_lock);

    // release success then return true 
    f->eax = true;

}

/* just find free sema in table and then allocate it and return its idx */
static void sys_sema_init(struct intr_frame* f, uint32_t* args) {
    if (!verify_user_range(&args[1], sizeof(char*))) {
        printf("come 892\n");
        process_exit(-1);
    }
    char* u_sema = (char*)args[1];
    if (u_sema == NULL)
    {
        f->eax = false;
        return;
    }
    
    // check one byte pointed by u_sema if valid 
    if (!verify_user_range(u_sema, sizeof(char))) {
        process_exit(-1);
    }


    if (!verify_user_range(&args[2], sizeof(int))) {
        process_exit(-1);
    }
    int initial_val = (int)args[2];
    if (initial_val < 0)
    {
        f->eax = false;
        return;
    }


    lock_acquire(&table_lock);

    // look for free sema_entry
    int idx = -1;
    for (int i = 0; i < MAX_USER_SEMAS; i++) {
        if (!sema_table[i].in_use) {
            idx = i;
            break;
        }
    }

    if (idx == -1) {
        // semaphore table is full 
        lock_release(&table_lock);
        f->eax = false;
        return;
    }

    // init kernel sema 
    sema_init(&sema_table[idx].kernel_sema, initial_val);
    sema_table[idx].in_use = true;
    sema_table[idx].owner_pid = get_pid(thread_current()->pcb);

    lock_release(&table_lock);

    // write index back to userspace 
    *u_sema = (char)idx;

    f->eax = true;
}

/* get arguments and check validation and then check valid for 
    semaphore in kernel sema table and finally call sema_down */
static void sys_sema_down(struct intr_frame* f, uint32_t* args) {

    if (!verify_user_range(&args[1], sizeof(char*))) {
        process_exit(-1);
    }
    char* u_sema = (char*)args[1];

    if (u_sema == NULL)
    {
        f->eax = false;
        return;
    }

    if (!verify_user_range(u_sema, sizeof(char))) {
        process_exit(-1);
    }

    int idx = (int)*u_sema;


    lock_acquire(&table_lock);

    // check id range, if it is registered and belongs to current process 
    if (idx < 0 || idx >= MAX_USER_SEMAS ||
        !sema_table[idx].in_use ||
        sema_table[idx].owner_pid != get_pid(thread_current()->pcb)) {
        lock_release(&table_lock);
        f->eax = false;
        return;
    }

    // must release here because sema_down maybe block 
    lock_release(&table_lock);


    sema_down(&sema_table[idx].kernel_sema);
    f->eax = true;
}

/* check arguments and then check validation of semaphore and finally call sema_up */
static void sys_sema_up(struct intr_frame* f, uint32_t* args) {

    if (!verify_user_range(&args[1], sizeof(char*))) {
        process_exit(-1);
    }
    char* u_sema = (char*)args[1];
    if (u_sema == NULL)
    {
        f->eax = false;
        return;
    }

    if (!verify_user_range(u_sema, sizeof(char))) {
        process_exit(-1);
    }

    int idx = (int)*u_sema;

    
    lock_acquire(&table_lock);

    if (idx < 0 || idx >= MAX_USER_SEMAS ||
        !sema_table[idx].in_use ||
        sema_table[idx].owner_pid != get_pid(thread_current()->pcb)) {
        lock_release(&table_lock);
        f->eax = false;
        return;
    }

    lock_release(&table_lock);

    
    sema_up(&sema_table[idx].kernel_sema);
    f->eax = true;
}

/* get tid from current user thread's corresponding kernel thread */
static void sys_get_tid(struct intr_frame* f, uint32_t* args) {
    f->eax = thread_current()->tid;
    return;
}






