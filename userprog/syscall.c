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



struct lock filesys_lock;   // protect file system operation global lock

static bool verify_user_range(const void *uaddr, size_t size);
static bool verify_user_string(const char *str);


static void syscall_handler(struct intr_frame*);

void syscall_init(void) { 
    intr_register_int(0x30, 3, INTR_ON, syscall_handler, "syscall");
    lock_init(&filesys_lock);
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
        {
            if (!verify_user_range((void*)(f->esp + 4), sizeof(int))) {
                process_exit(-1);
                return;
            }
            int status = *(int*)(f->esp + 4);
            f->eax = status;
            

            process_exit(status);
            break;
        }
        
    case SYS_PRACTICE:
        {
            if (!verify_user_range((void*)(f->esp + 4), sizeof(int))) {
                process_exit(-1);
                return;
            }
            int i = *(int*)(f->esp + 4);
            f->eax = i + 1;
            break;
        }
        
    case SYS_EXEC:
        {    
            if (!verify_user_range((void*)(f->esp + 4), sizeof(char*))) {
                process_exit(-1);
                return;
            }
            char* cmd_line = *(char**)(f->esp + 4);

            if (!verify_user_string(cmd_line)) {
                process_exit(-1);
                return;
            }
            f->eax = process_execute(cmd_line);
            break;
        }
        
    case SYS_WAIT:
        {
            if (!verify_user_range((void*)(f->esp + 4), sizeof(int))) {
                process_exit(-1);
                return;
            }
            pid_t pid = *(pid_t*)(f->esp + 4);
            f->eax = process_wait(pid);
            break;
        }
    
    case SYS_FORK:
        {     
            f->eax = process_fork(f);
            break;
        }

    case SYS_CREATE:
        {    
            // verify argument file name pointer itself
            if (!verify_user_range(&args[1], sizeof(char*))) {
                process_exit(-1);
                break;
            }
            char *file = (char*)args[1];
            // verify file name string 
            if (!verify_user_string(file)) {
                process_exit(-1);
                break;
            }
            // verify initial size argument
            if (!verify_user_range(&args[2], sizeof(unsigned))) {
                process_exit(-1);
                break;
            }
            unsigned initial_size = (unsigned)args[2];
            
            lock_acquire(&filesys_lock);
            bool ok = filesys_create(file, initial_size);
            lock_release(&filesys_lock);
            f->eax = ok;

            break;
        }

    case SYS_REMOVE:
        {    
            // verify argument file name pointer itself
            if (!verify_user_range(&args[1], sizeof(char *))) {
                process_exit(-1);
                break;
            }
            char *file = *(char **)&args[1];
            // verify file name string 
            if (!verify_user_string(file)) {
                process_exit(-1);
                break;
            }

            lock_acquire(&filesys_lock);
            bool ok = filesys_remove(file);
            lock_release(&filesys_lock);

            f->eax = ok;

            break;
        }

    case SYS_OPEN:
        {    
            // verify argument file name pointer itself
            if (!verify_user_range(&args[1], sizeof(char *))) {
                process_exit(-1);
                break;
            }
            char *file = *(char **)&args[1];
            // verify file name string 
            if (!verify_user_string(file)) {
                process_exit(-1);
                break;
            }

            lock_acquire(&filesys_lock);
            struct file *f_ptr = filesys_open(file);
            if (f_ptr == NULL) {
                lock_release(&filesys_lock);
                f->eax = -1;
                break;
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


            break;
        }

    case SYS_FILESIZE:
        {    
            // verify fd 
            if (!verify_user_range(&args[1], sizeof(int))) {
                process_exit(-1);
                break;
            }
            int fd = args[1];
            struct thread *cur = thread_current();
            struct process *pcb = cur->pcb;
            
            // check fd validation 
            if (fd < 0 || fd >= MAX_FILES || pcb->files[fd] == NULL) {
                f->eax = -1;
                break;
            }
            
            lock_acquire(&filesys_lock);
            off_t file_size = file_length(pcb->files[fd]);
            lock_release(&filesys_lock);
            f->eax = file_size;
            break;
        }

    case SYS_READ:
        {    
            // verify fd
            if (!verify_user_range(&args[1], sizeof(int))) {
                process_exit(-1);
                break;
            }
            int fd = args[1];
            // verify buffer pointer
            if (!verify_user_range(&args[2], sizeof(void *))) {
                process_exit(-1);
                break;
            }
            void *buffer = *(void **)&args[2];
            // verify size
            if (!verify_user_range(&args[3], sizeof(unsigned))) {
                process_exit(-1);
                break;
            }
            unsigned read_size = args[3];
            // verify buffer writable (whole block)
            if (!verify_user_range(buffer, read_size)) {
                process_exit(-1);
                break;
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
            break;
        }

    case SYS_WRITE:
        {    
            // verify fd
            if (!verify_user_range(&args[1], sizeof(int))) {
                process_exit(-1);
                break;
            }
            int fd = args[1];
            // verify buffer pointer
            if (!verify_user_range(&args[2], sizeof(void *))) {
                process_exit(-1);
                break;
            }
            const void *buffer = *(const void **)&args[2];
            // verify size
            if (!verify_user_range(&args[3], sizeof(unsigned))) {
                process_exit(-1);
                break;
            }
            unsigned write_size = args[3];
            // verify buffer readable (whole block)
            if (!verify_user_range((void *)buffer, write_size)) {
                process_exit(-1);
                break;
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
            break;
        }
    case SYS_SEEK:
        {    
            // verify fd
            if (!verify_user_range(&args[1], sizeof(int))) {
                process_exit(-1);
                break;
            }
            int fd = args[1];
            // verify position
            if (!verify_user_range(&args[2], sizeof(unsigned))) {
                process_exit(-1);
                break;
            }
            unsigned position = args[2];
            
            struct thread *cur = thread_current();
            struct process *pcb = cur->pcb;
            
            if (fd < 0 || fd >= MAX_FILES || pcb->files[fd] == NULL) {
                // if fd invalid do nothing
                break;
            }
            
            lock_acquire(&filesys_lock);
            file_seek(pcb->files[fd], position);
            lock_release(&filesys_lock);
            break;
        }

    case SYS_TELL:
        {
            // verify fd
            if (!verify_user_range(&args[1], sizeof(int))) {
                process_exit(-1);
                break;
            }
            int fd = args[1];
            struct thread *cur = thread_current();
            struct process *pcb = cur->pcb;
            
            if (fd < 0 || fd >= MAX_FILES || pcb->files[fd] == NULL) {
                f->eax = -1;  // just return -1
                break;
            }
            
            lock_acquire(&filesys_lock);
            off_t pos = file_tell(pcb->files[fd]);
            lock_release(&filesys_lock);
            f->eax = pos;
            break;
        }

    case SYS_CLOSE:
        {    
            // verify fd
            if (!verify_user_range(&args[1], sizeof(int))) {
                process_exit(-1);
                break;
            }
            int fd = args[1];
            struct thread *cur = thread_current();
            struct process *pcb = cur->pcb;
            
            if (fd < 0 || fd >= MAX_FILES || pcb->files[fd] == NULL) {
                f->eax = -1;  // just return -1 if fd invalid
                break;
            }
            
            lock_acquire(&filesys_lock);
            file_close(pcb->files[fd]);
            pcb->files[fd] = NULL;
            lock_release(&filesys_lock);
            break;
        }
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
    if (pagedir_get_page(pd, start) == NULL)
        return false;

    // check the end byte if is in the page
    if (pagedir_get_page(pd, end - 1) == NULL)
        return false;

    return true;
}


static bool verify_user_string(const char *str) {
    if (!is_user_vaddr(str))
        return false;

    uint32_t *pd = thread_current()->pcb->pagedir;

    while (1) {
        // check the current str pointer if in mapped page
        if (pagedir_get_page(pd, str) == NULL)
            return false;

        // seek \0 in current page
        const char *page_end = (const char *)(((uint32_t)str | PGMASK) + 1); // get the first byte address in next page
        while (str < page_end) {
            if (*str == '\0')  // page has been checked so deference is safe
                return true;
            str++;
        }
        // not find \0 in current page so go to next page
    }
}