#include "userprog/syscall.h"
#include "user/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "lib/string.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "devices/shutdown.h"
#include "devices/input.h"
#include "process.h"
#include "threads/vaddr.h"
#include "pagedir.h"
#include "threads/synch.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "kernel/console.h"
#define ARRAY_LEN(X) (sizeof(X) / sizeof((X)[0]))
static struct lock filesys_lock;
static struct intr_frame *if_;
extern struct list all_list;
static void syscall_handler (struct intr_frame *);
static void check_addr_validity(void *addr) {
  if (is_kernel_vaddr(addr) || pagedir_get_page(thread_current()->pagedir, addr) == NULL) {
    exit_from_user_process(-1);
  }
}
// 以int为单位返回参数，n代表第几个参数，从0到3，第0个参数是系统调用号
static int32_t argint(int n) {
  ASSERT (n >= 0 && n <= 3);
  void *addr = if_->esp + n * sizeof(int);
  // 参数本身存储在用户空间中的地址也要检查
  for (size_t i = 0; i < sizeof(int32_t); i++) {
    check_addr_validity(addr + i);
  }
  return *(int32_t*)addr;
}
static void *argaddr(int n) {
  void *addr = (void*)argint(n);
  // 每一个地址都要检查
  for (size_t i = 0;; i++) {
    check_addr_validity(addr + i);  
    if (*(char*)(addr + i) == 0) break;
  }
  return addr;
}
static uint32_t get_sys_num(void) {
  return argint(0);
}
static uint32_t arg_fd(int n) {
  uint32_t fd = argint(n);
  if (fd >= FDNUM) exit_from_user_process(-1);
  return fd;
}
static uint32_t sys_halt(void) {
  // printf("************FUCK YOU PINTOS***************\n");
  shutdown_power_off(); 
  return 0;
}
static uint32_t sys_exit(void){
  int exit_state = argint(1);
  exit_from_user_process(exit_state);
  NOT_REACHED ();
} 
static uint32_t sys_exec(void){
  char *cmd_line = (char*)argaddr(1);
  lock_acquire(&filesys_lock);
  int32_t res = process_execute(cmd_line);
  lock_release(&filesys_lock);
  // if (res == TID_ERROR) exit_from_user_process(-1);
  return res;
} 
static uint32_t sys_wait(void){
  pid_t pid = argint(1);
  return process_wait(pid);
} 
static uint32_t sys_create(void){
  const char *file = argaddr(1);
  uint32_t initial_size = argint(2);
  lock_acquire(&filesys_lock);
  // printf("fuck\n");
  uint32_t ret = (uint32_t)filesys_create(file, initial_size);
  // printf("fuck you\n");
  lock_release(&filesys_lock);
  return ret;
} 
static uint32_t sys_remove(void){
  const char *file = argaddr(1);
  lock_acquire(&filesys_lock);
  uint32_t ret = (uint32_t)filesys_remove(file);
  lock_release(&filesys_lock);
  return ret;
} 
static bool 
is_running_exefile(const char *name) {
  for (struct list_elem *e = list_begin(&all_list); e != list_end(&all_list); e = list_next(e)) {
    struct thread *t = list_entry(e, struct thread, allelem);
    if (strcmp(t->name, name) == 0) return true;
  }
  return false;
}
// 打开一个指定名字的文件并且分配文件描述符,0和1保留给控制台标准io
// 与unix语义不同，pintos中的进程打开文件表不继承给子进程
static uint32_t sys_open(void){
  const char *file_name = argaddr(1);
  lock_acquire(&filesys_lock);
  struct file * f= filesys_open(file_name);
  lock_release(&filesys_lock);
  if (f == NULL) return -1;
  // 阻止向正在运行的可执行文件写入
  if (is_running_exefile(file_name)) file_deny_write(f);
  return fdalloc(f);
} 
static uint32_t sys_filesize(void){
  int fd = arg_fd(1);
  lock_acquire(&filesys_lock);
  struct file * f = thread_current()->ofile[fd];
  uint32_t res = file_length(f); 
  lock_release(&filesys_lock);
  return res;
}
// 从指定的fd中读取size字节到buffer中，如果fd是0，从标准输入键盘中读取
static uint32_t sys_read(void){
  int fd = arg_fd(1);
  void* buffer = argaddr(2);
  uint32_t size = argint(3);
  lock_acquire(&filesys_lock);
  struct file *f = thread_current()->ofile[fd];
  int res;
  // 如果fd是0，使用input_getc从键盘读取输入
  if (fd == 0) res = input_getc();
  else if (fd == 1) exit_from_user_process(-1);
  else res = file_read(f, buffer, size);
  lock_release(&filesys_lock);
  return res;
}
// 从buffer中向指定的fd写入size字节，如果fd是1，写到控制台上；写到了文件的末尾通常会扩展文件，但是此时（p2）还没有实现
// 现在是尽可能地写直到末尾停止
static uint32_t sys_write(void){
  int fd = arg_fd(1);
  void* buffer = argaddr(2);
  uint32_t size = argint(3);
  struct file *f = thread_current()->ofile[fd];
  uint32_t res;
  if (fd == 1) {
    putbuf(buffer, size);
    res = size;
  } else if (fd == 0) {
    exit_from_user_process(-1);
  } else {
    lock_acquire(&filesys_lock);
    res = file_write(f, buffer, size);
    lock_release(&filesys_lock);
  }
  return res;
}
// 改变指定fd对应的打开文件结构体中的读取指针，也就是下一次读写的起点
// 如果seek超过了文件的末尾不是error，以后读取到0字节，写入会扩展文件（中间的gap用0填充）
// 但是此时pintos文件系统的文件长度是固定的，所以写超过末尾会返回error
static uint32_t sys_seek(void){
  int fd = arg_fd(1);
  uint32_t position = argint(2);
  lock_acquire(&filesys_lock);
  file_seek(thread_current()->ofile[fd], position);
  lock_release(&filesys_lock);
  return 0;
}
static uint32_t sys_tell(void){
  int fd = arg_fd(1);
  lock_acquire(&filesys_lock);
  int res = file_tell(thread_current()->ofile[fd]);
  lock_release(&filesys_lock);
  return res;
}
// 关闭指定的文件描述符（关不关file结构体？），
// 进程exit或终止的时候要通过调用这个函数，关闭它所有的打开文件描述符
static uint32_t sys_close(void){
  int fd = arg_fd(1);
  lock_acquire(&filesys_lock);
  file_close(thread_current()->ofile[fd]);
  thread_current()->ofile[fd] = NULL;
  lock_release(&filesys_lock);
  return 0;
}
// static uint32_t sys_mmap(void){}
// static uint32_t sys_munmap(void){}
// static uint32_t sys_chdir(void){}
// static uint32_t sys_mkdir(void){}
// static uint32_t sys_readdir(void){}
// static uint32_t sys_isdir(void){}
// static uint32_t sys_inumber(void){}
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
  lock_init(&filesys_lock);
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
syscall_handler (struct intr_frame *f UNUSED) 
{
  ASSERT (f != NULL);
  if_ = f;
  uint32_t sys_num = get_sys_num();
  if (sys_num >= ARRAY_LEN(syscalls)) exit_from_user_process(-1);
  if_->eax = syscalls[sys_num]();
}
