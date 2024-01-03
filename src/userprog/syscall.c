#include "filesys/inode.h"
#include "filesys/directory.h"
#include "threads/palloc.h"
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
struct lock filesys_lock;
extern struct list all_list;
static void syscall_handler (struct intr_frame *);
static void check_addr_validity(void *addr) {
  // pagedir_get_page(thread_current()->pagedir, addr);
  if (is_kernel_vaddr(addr)) {
    exit_from_user_process(-1);
  }
}
// 以int为单位返回参数，n代表第几个参数，从0到3，第0个参数是系统调用号
static int32_t argint(int n) {
  ASSERT (n >= 0 && n <= 3);
  void *addr = thread_current()->user_esp + n * sizeof(int);
  // 参数本身存储在用户空间中的地址也要检查
  for (size_t i = 0; i < sizeof(int32_t); i++) {
    check_addr_validity(addr + i);
  }
  return *(int32_t*)addr;
}
// 获取系统调用参数中的指针并且检查指针所指向的空间是否可以访问，如果不能访问需要在系统调用的入口触发pagefault
// 只有需要访问指针指向的空间的系统调用才需要进行检查，如read write exec open等；对于mmap，不需要进行检查，所以
// 获取了地址后直接返回即可
static void *argaddr(int n, bool check, int str_len) {
  void *addr = (void*)argint(n);
  if (!check) return addr;
  // 每一个地址都要检查
  for (size_t i = 0;; i++) {
    check_addr_validity(addr + i);  
    // 系统调用传入的地址（比如read write传入的buffer和exec open传入的filename）要全部遍历一次
    // 将它们的pagefault在系统调用的入口处触发，不能在执行到一半时触发，否则会造成死锁
    if (*(char*)(addr + i) == 0 && str_len == -1) break;
    if (str_len != -1 && i == str_len) break;
    // 触发完pagefault之后还要将该page pin住，在整个系统调用的过程中不能被驱逐出去，否则还是会
    // 出现在内核中pagefault的情况
    if (i == 0) pin_frame(thread_current()->pagedir, addr);
    else if (pg_ofs(addr + i) == 0) pin_frame(thread_current()->pagedir, addr + i);
  }
  return addr;
}
static void
unpin_user_frame(int n, int str_len) {
  void *addr = (void*)argint(n);
  for (size_t i = 0;; i++) {
    if (str_len == -1 && *(char*)(addr + i) == 0) break;
    if (str_len != -1 && i == str_len) break;
    if (i == 0) unpin_frame(thread_current()->pagedir, addr);
    else if (pg_ofs(addr + i) == 0) unpin_frame(thread_current()->pagedir, addr + i);
  }
}
static uint32_t get_sys_num(void) {
  return argint(0);
}
static uint32_t arg_fd(int n) {
  uint32_t fd = argint(n);
  if (fd >= FDNUM) exit_from_user_process(-1);
  return fd;
}
static int32_t sys_halt(void) {
  // printf("************FUCK YOU PINTOS***************\n");
  shutdown_power_off(); 
  return 0;
}
static int32_t sys_exit(void){
  int exit_state = argint(1);
  exit_from_user_process(exit_state);
  NOT_REACHED ();
} 
static int32_t sys_exec(void){
  char *cmd_line = (char*)argaddr(1, true, -1);
  // printf("%s\n",cmd_line);
  lock_acquire(&filesys_lock);
  int32_t res = process_execute(cmd_line);
  lock_release(&filesys_lock);
  unpin_user_frame(1, -1);
  return res;
} 
static int32_t sys_wait(void){
  pid_t pid = argint(1);
  return process_wait(pid);
} 
static int32_t sys_create(void){
  const char *file = argaddr(1, true, -1);
  uint32_t initial_size = argint(2);
  lock_acquire(&filesys_lock);
  // printf("fuck\n");
  uint32_t ret = (uint32_t)filesys_create(file, initial_size, false);
  // printf("fuck you\n");
  lock_release(&filesys_lock);
  unpin_user_frame(1, -1);
  return ret;
} 
static int32_t sys_remove(void){
  const char *file = argaddr(1, true, -1);
  lock_acquire(&filesys_lock);
  uint32_t ret = (uint32_t)filesys_remove(file);
  lock_release(&filesys_lock);
  unpin_user_frame(1, -1);
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
static int32_t sys_open(void){
  const char *file_name = argaddr(1, true, -1);
  lock_acquire(&filesys_lock);
  struct file * f= filesys_open(file_name);
  lock_release(&filesys_lock);
  if (f == NULL) {
    unpin_user_frame(1, -1);
    return -1;
  }
  // 阻止向正在运行的可执行文件写入
  if (is_running_exefile(file_name)) file_deny_write(f);
  unpin_user_frame(1, -1);
  uint32_t fd = fdalloc(f);
  return fd;
} 
static int32_t sys_filesize(void){
  int fd = arg_fd(1);
  struct file * f = thread_current()->ofile[fd];
  lock_acquire(&filesys_lock);
  uint32_t res = file_length(f); 
  lock_release(&filesys_lock);
  return res;
}
// 从指定的fd中读取size字节到buffer中，如果fd是0，从标准输入键盘中读取
static int32_t sys_read(void){
  int fd = arg_fd(1);
  uint32_t size = argint(3);
  void* buffer = argaddr(2, true, size);
  // 禁止用户往栈上写（管得真鸡巴宽）
  if (buffer < pg_round_down(thread_current()->user_esp) && buffer > pg_round_down(thread_current()->user_esp) - PGSIZE * 10) {
    exit_from_user_process(-1);
  }
  struct file *f = thread_current()->ofile[fd];
  int res;
  // 如果fd是0，使用input_getc从键盘读取输入
  if (fd == 0) res = input_getc();
  else if (fd == 1) exit_from_user_process(-1);
  else {
    lock_acquire(&filesys_lock);
    res = file_read(f, buffer, size);
    lock_release(&filesys_lock);
  }
  unpin_user_frame(2, size);
  return res;
}
// 从buffer中向指定的fd写入size字节，如果fd是1，写到控制台上；写到了文件的末尾通常会扩展文件，但是此时（p2）还没有实现
// 现在是尽可能地写直到末尾停止
static int32_t sys_write(void){
  int fd = arg_fd(1);
  uint32_t size = argint(3);
  void* buffer = argaddr(2, true, size);
  struct file *f = thread_current()->ofile[fd];
  int32_t res;
  if (fd == 1) {
    putbuf(buffer, size);
    res = size;
  } else if (fd == 0) {
    exit_from_user_process(-1);
  } else if (is_inode_dir(f->inode)) {
    exit_from_user_process(-1); 
  } else {
    lock_acquire(&filesys_lock);
    res = file_write(f, buffer, size);
    lock_release(&filesys_lock);
  }
  unpin_user_frame(2, size);
  return res;
}
// 改变指定fd对应的打开文件结构体中的读取指针，也就是下一次读写的起点
// 如果seek超过了文件的末尾不是error，以后读取到0字节，写入会扩展文件（中间的gap用0填充）
// 但是此时pintos文件系统的文件长度是固定的，所以写超过末尾会返回error
static int32_t sys_seek(void){
  int fd = arg_fd(1);
  uint32_t position = argint(2);
  lock_acquire(&filesys_lock);
  file_seek(thread_current()->ofile[fd], position);
  lock_release(&filesys_lock);
  return 0;
}
static int32_t sys_tell(void){
  int fd = arg_fd(1);
  lock_acquire(&filesys_lock);
  int res = file_tell(thread_current()->ofile[fd]);
  lock_release(&filesys_lock);
  return res;
}
// 关闭指定的文件描述符（关不关file结构体？），
// 进程exit或终止的时候要通过调用这个函数，关闭它所有的打开文件描述符
static int32_t sys_close(void){
  int fd = arg_fd(1);
  lock_acquire(&filesys_lock);
  file_close(thread_current()->ofile[fd]);
  lock_release(&filesys_lock);
  thread_current()->ofile[fd] = NULL;
  return 0;
}
static int32_t exit_from_user(void) {
  exit_from_user_process(-1);
  NOT_REACHED();
}
static int32_t sys_mmap(void){
  int fd = arg_fd(1);
  // mmap不需要访问传入参数中的指针所指向的位置，所以获取参数中的地址时不需要检查
  void *addr = argaddr(2, false, -1);
  int mapid = create_mmap_vm_area_struct(fd, addr);
  return mapid;
}

static int32_t sys_munmap(void) {
  int mapping = argint(1);
  lock_acquire(&thread_current()->mm->lock);
  struct list_elem* e= find_mmap_vm_area_struct(mapping);
  list_remove(e);
  destory_mmap_vas(list_entry(e, struct vm_area_struct, vm_area_list_elem)); 
  lock_release(&thread_current()->mm->lock);
  return 0;
}
static int32_t sys_chdir(void) {
  char *path = (char*)argaddr(1, true, -1);
  struct dir *dir = name_inode(path);
  if (dir == NULL) {
    dir_close(dir);
    return false;
  }
  thread_current()->cwd = dir;
  unpin_user_frame(1, -1);
  return true;
}
static int32_t sys_mkdir(void) {
  char * path = (char*)argaddr(1, true, -1);
  uint32_t success = (uint32_t)filesys_create(path, 0, true);
  if (success) {
    struct dir *dir = name_inode(path);
    char temp[NAME_MAX + 1];
    struct dir *parent_dir = name_inode_parent(path, temp);
    ASSERT(dir != NULL && is_inode_dir(dir_get_inode(dir)));
    enum intr_level old_level = intr_disable();
    // 不加nlink计数
    dir_add(dir, ".", dir_get_inode(dir)->sector);
    dir_add(dir, "..", dir_get_inode(parent_dir)->sector);
    intr_set_level(old_level);
    dir_close(dir);
    dir_close(parent_dir);
  }
  unpin_user_frame(1, -1);
  return success;
}
static int32_t sys_readdir(void) {
  int fd = arg_fd(1);
  char *name = (char*)argaddr(2, true, READDIR_MAX_LEN + 1);
  struct file *file = thread_current()->ofile[fd];
  ASSERT(is_inode_dir(file->inode)); 

  struct dir *dir = dir_open(inode_reopen(file->inode));
  dir->pos = file->pos;
  if (dir->pos == 0) {
    ASSERT(dir_readdir(dir, name) && strcmp(name, ".") == 0);
    ASSERT(dir_readdir(dir, name) && strcmp(name, "..") == 0);
  }
  bool success = dir_readdir(dir, name);
  file->pos = dir->pos;
  dir_close(dir);
  unpin_user_frame(2, READDIR_MAX_LEN + 1);
  return success;
}
static int32_t sys_isdir(void) {
  int fd = arg_fd(1);
  return is_inode_dir(thread_current()->ofile[fd]->inode);
}
static int32_t sys_inumber(void) {
  int fd = arg_fd(1);
  return inode_get_inumber(thread_current()->ofile[fd]->inode);
}
static int32_t (*syscalls[])(void) = {
  [EXIT_FROM_USER] exit_from_user,
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
  [SYS_MMAP] sys_mmap,
  [SYS_MUNMAP] sys_munmap,
  [SYS_CHDIR] sys_chdir,
  [SYS_MKDIR] sys_mkdir,
  [SYS_READDIR] sys_readdir,
  [SYS_ISDIR] sys_isdir,
  [SYS_INUMBER] sys_inumber
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
  // printf("SYSCALL*********\n");
  ASSERT (f != NULL);
  thread_current()->user_esp = f->esp;
  // 如果随便移动esp，会导致系统调用号为0，调用exit
  uint32_t sys_num = get_sys_num();
  if (sys_num >= ARRAY_LEN(syscalls)) exit_from_user_process(-1);
  f->eax = syscalls[sys_num]();
  thread_current()->user_esp = NULL;
}
