#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "devices/shutdown.h"
#define ARRAY_LEN(X) (sizeof(X) / sizeof((X)[0]))

static struct intr_frame *if_;
static void syscall_handler (struct intr_frame *);

static uint32_t sys_halt(void) {
  printf("************FUCK YOU PINTOS***************\n");
  shutdown_power_off(); 
  return 0;
}
static uint32_t sys_exit(void){} 
static uint32_t sys_exec(void){} 
static uint32_t sys_wait(void){} 
static uint32_t sys_create(void){} 
static uint32_t sys_remove(void){} 
static uint32_t sys_open(void){} 
static uint32_t sys_filesize(void){}
static uint32_t sys_read(void){}
static uint32_t sys_write(void){}
static uint32_t sys_seek(void){}
static uint32_t sys_tell(void){}
static uint32_t sys_close(void){}
static uint32_t sys_mmap(void){}
static uint32_t sys_munmap(void){}
static uint32_t sys_chdir(void){}
static uint32_t sys_mkdir(void){}
static uint32_t sys_readdir(void){}
static uint32_t sys_isdir(void){}
static uint32_t sys_inumber(void){}

static uint32_t get_sys_num(void) {
  uint32_t esp = if_->esp;
  hex_dump(esp, esp, 12, true);
  uint32_t sys_num = *((uint32_t*)esp);
  return sys_num;
}
static uint32_t (*syscalls[])(void) = {
  [SYS_HALT] sys_halt,
  [SYS_EXIT] sys_exit,
  [SYS_EXEC] sys_exec,
  [SYS_WAIT] sys_wait,
  [SYS_CREATE] sys_create,
  [SYS_REMOVE] sys_remove,
  [SYS_OPEN] sys_open,
  [SYS_FILESIZE] sys_filesize,
  [SYS_READ] sys_read,
  [SYS_WRITE] sys_write,
  [SYS_SEEK] sys_seek,
  [SYS_TELL] sys_tell,
  [SYS_CLOSE] sys_close,
};
void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
syscall_handler (struct intr_frame *f UNUSED) 
{
  ASSERT (f != NULL);
  if_ = f;
  uint32_t sys_num = get_sys_num();
  printf("************%d***************\n", sys_num);
  ASSERT (sys_num >= 0 && sys_num < ARRAY_LEN(syscalls) && syscalls[sys_num]);
  if_->eax = syscalls[sys_num]();
  printf ("system call!\n");
  // thread_exit ();
}
