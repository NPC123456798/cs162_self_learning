#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/process.h"
#include "userprog/pagedir.h"
#include "devices/shutdown.h"

static void syscall_handler(struct intr_frame*);

void syscall_init(void) { intr_register_int(0x30, 3, INTR_ON, syscall_handler, "syscall"); }

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
        shutdown_power_off();
        break;
    case SYS_EXIT:

        if (!verify_user_range((void*)(f->esp + 4), sizeof(int))) {
            process_exit(-1);
            return;
        }
        int status = *(int*)(f->esp + 4);
        f->eax = status;
        printf("%s: exit(%d)\n", thread_current()->pcb->process_name, status);
        process_exit(status);
        break;
        
    case SYS_PRACTICE:

        if (!verify_user_range((void*)(f->esp + 4), sizeof(int))) {
            process_exit(-1);
            return;
        }
        int i = *(int*)(f->esp + 4);
        f->eax = i + 1;
        break;
        
    case SYS_EXEC:
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
        
    case SYS_WAIT:

        if (!verify_user_range((void*)(f->esp + 4), sizeof(int))) {
            process_exit(-1);
            return;
        }
        pid_t pid = *(pid_t*)(f->esp + 4);
        f->eax = process_wait(pid);
        break;
    
    case SYS_FORK:
        f->eax = process_fork(f);
        break;

    case SYS_CREATE:
    // TODO: implement create
        break;

    case SYS_REMOVE:
        // TODO: implement remove
        break;

    case SYS_OPEN:
        // TODO: implement open
        break;

    case SYS_FILESIZE:
        // TODO: implement filesize
        break;

    case SYS_READ:
        // TODO: implement read
        break;

    case SYS_WRITE:
        // TODO: implement write
        break;

    case SYS_SEEK:
        // TODO: implement seek
        break;

    case SYS_TELL:
        // TODO: implement tell
        break;
        
    case SYS_CLOSE:
        // TODO: implement close
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


static bool verify_user_range(const void *uaddr, size_t size) {
    if (!is_user_vaddr(uaddr))
        return false;

    uint8_t *start = (uint8_t *)uaddr;
    uint8_t *end = start + size;
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