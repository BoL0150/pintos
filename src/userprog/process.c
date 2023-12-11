#include "lib/string.h"
#include "filesys/inode.h"
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
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "threads/malloc.h"
#include "string.h"
#include "devices/timer.h"
#include "threads/synch.h"
#include "threads/pte.h"

extern struct lock filesys_lock;

static thread_func start_process NO_RETURN;
static bool load (const char *cmdline, void (**eip) (void), void **esp);
static void create_vm_area_struct(uint32_t upage, uint32_t zero_bytes, uint32_t read_bytes, struct file *file, off_t ofs, bool writable, bool is_stack);

static uint32_t pass_argument(const char **argv) {
  uint32_t sp = PHYS_BASE;
  uint32_t u_arg_address[MAXARG];
  int argc;
  for (argc = 0; argv[argc]; argc++) {
    sp -= strlen(argv[argc]) + 1;
    sp -= sp % 4;
    strlcpy((char*)sp, argv[argc], strlen(argv[argc]) + 1); // strlcpy的size是字符串长度加终止符
    u_arg_address[argc] = sp;
  }
  u_arg_address[argc] = 0;
  sp -= (argc + 1) * sizeof(uint32_t);
  sp -= sp % 4;
  strlcpy((char*)sp, (char*)u_arg_address, (argc + 1) * sizeof(uint32_t));
  sp -= 4;
  *(uint32_t *)sp = sp + 4; // argv地址
  sp -= 4;
  *(uint32_t *)sp = argc;
  sp -= 4;
  *(uint32_t *)sp = 0; // fake return address
  return sp;
}
static char ** split_command_line(char *command_line) {
  // command_line的大小不会超过128字节，我使用给它分配的后半的page存指针
  char **argv = (char**)(command_line + PGSIZE / 2);
  char *save_ptr;
  int argc = 0;
  for (char *token = strtok_r(command_line, " ", &save_ptr); token != NULL; token = strtok_r(NULL, " ", &save_ptr)) {
      argv[argc++] = token; 
  }
  argv[argc] = 0;
  return argv;
}
/** Starts a new thread running a user program loaded from
   FILENAME.  The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   thread id, or TID_ERROR if the thread cannot be created. */
tid_t
process_execute (const char *command_line) 
{
  char *fn_copy;
  tid_t tid;
  ASSERT (strlen(command_line) < PGSIZE / 2);
  /* Make a copy of command_line.
     Otherwise there's a race between the caller and load(). */
  fn_copy = palloc_get_page (0);
  if (fn_copy == NULL)
    return TID_ERROR;
  strlcpy (fn_copy, command_line, PGSIZE);

  char **argv = split_command_line(fn_copy);
  bool load_success = true;
  struct semaphore sema;
  sema_init(&sema, 0);
  void* args[3] = {argv, &load_success, &sema};
  /* Create a new thread to execute command_line. */
  tid = thread_create (argv[0], PRI_DEFAULT, start_process, args);
  // thread_create出现error，由当前进程释放fn_copy；
  // 如果thread_create成功了，由子进程释放fn_copy,当前进程不需要重复释放
  if (tid == TID_ERROR) { 
    palloc_free_page (fn_copy);
  }
  if (tid != TID_ERROR) {
    sema_down(&sema); // 如果成功创建了新线程，还要等新线程成功load可执行文件才能返回
    tid = load_success ? tid : TID_ERROR; // thread_create没有error，但是load出现了error，
  }
  return tid;
}

/** A thread function that loads a user process and starts it
   running. */
static void
start_process (void *args)
{
  ASSERT(args != NULL);
  void **args_array = (void**)args;
  const char **argv = (const char**)args_array[0];
  bool *load_success= (bool*)args_array[1];
  struct semaphore *sema = (struct semaphore*)args_array[2];

  const char *file_name = argv[0];
  struct intr_frame if_;
  bool success;

  /* Initialize interrupt frame and load executable. */
  memset (&if_, 0, sizeof if_);
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;

  success = load (file_name, &if_.eip, &if_.esp);
  // printf("%s;success?%d\n",file_name, success);
  *load_success = success;

  // 加载完成，唤醒父进程
  sema_up(sema); 
  // 如果加载成功，向进程中传递命令行参数
  if (success) if_.esp = (void*)pass_argument(argv); 

  /* If load failed, quit. */
  palloc_free_page ((void*)file_name);
  if (!success) {
    thread_exit ();
  }

  /* Start the user process by simulating a return from an
     interrupt, implemented by intr_exit (in
     threads/intr-stubs.S).  Because intr_exit takes all of its
     arguments on the stack in the form of a `struct intr_frame',
     we just point the stack pointer (%esp) to our stack frame
     and jump to it. */
  asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g" (&if_) : "memory");
  NOT_REACHED ();
}

/** Waits for thread TID to die and returns its exit status.  If
   it was terminated by the kernel (i.e. killed due to an
   exception), returns -1.  If TID is invalid or if it was not a
   child of the calling process, or if process_wait() has already
   been successfully called for the given TID, returns -1
   immediately, without waiting.

   This function will be implemented in problem 2-2.  For now, it
   does nothing. */
int
process_wait (tid_t child_tid UNUSED) 
{
  struct list *child_list = &thread_current()->child_list;
  ASSERT(child_list != NULL);
  // ASSERT(!list_empty(child_list));
  for (struct list_elem *e = list_begin(child_list); e != list_end(child_list); e = list_next(e)) {
    struct thread_exit_state *tes = list_entry(e, struct thread_exit_state, child_list_elem);
    if (tes->tid != child_tid) continue;
    sema_down(&tes->sema);
    int exit_state = tes->exit_state;
    // wait完之后就可以把结构体删了，因为pintos规定不能在一个pid上wait多次
    list_remove(e);
    free(tes);
    return exit_state;
  }
  return -1;
}
// 用户进程导致的exit
void exit_from_user_process(int exit_state) {
  if (lock_held_by_current_thread(&filesys_lock)) lock_release(&filesys_lock);
  thread_current()->exit_state = exit_state;
  printf("%s: exit(%d)\n",thread_current()->name, exit_state);
  thread_exit();
  NOT_REACHED ();
}
/** Free the current process's resources. */
void
process_exit (void)
{
  struct thread *cur = thread_current ();
  uint32_t *pd;

  /* Destroy the current process's page directory and switch back
     to the kernel-only page directory. */
  pd = cur->pagedir;
  if (pd != NULL) 
    {
      /* Correct ordering here is crucial.  We must set
         cur->pagedir to NULL before switching page directories,
         so that a timer interrupt can't switch back to the
         process page directory.  We must activate the base page
         directory before destroying the process's page
         directory, or our active page directory will be one
         that's been freed (and cleared). */
      cur->pagedir = NULL;
      pagedir_activate (NULL);
      pagedir_destroy (pd);
    }
}

/** Sets up the CPU for running user code in the current
   thread.
   This function is called on every context switch. */
void
process_activate (void)
{
  struct thread *t = thread_current ();

  /* Activate thread's page tables. */
  pagedir_activate (t->pagedir);

  /* Set thread's kernel stack for use in processing
     interrupts. */
  tss_update ();
}

/** We load ELF binaries.  The following definitions are taken
   from the ELF specification, [ELF1], more-or-less verbatim.  */

/** ELF types.  See [ELF1] 1-2. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/** For use with ELF types in printf(). */
#define PE32Wx PRIx32   /**< Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32   /**< Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32   /**< Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16   /**< Print Elf32_Half in hexadecimal. */

/** Executable header.  See [ELF1] 1-4 to 1-8.
   This appears at the very beginning of an ELF binary. */
struct Elf32_Ehdr
  {
    unsigned char e_ident[16];
    Elf32_Half    e_type;    // 文件类型
    Elf32_Half    e_machine; // 机器名称
    Elf32_Word    e_version; 
    Elf32_Addr    e_entry;   // 文件装入内存中的起始地址
    Elf32_Off     e_phoff;   // program header offset 程序头表在文件中的偏移地址
    Elf32_Off     e_shoff;   // section header offset 节头表在文件中的偏移地址
    Elf32_Word    e_flags;
    Elf32_Half    e_ehsize;  // elf header size 当前的elf头的大小
    Elf32_Half    e_phentsize; // program header entry size 程序头表的每个表项的大小
    Elf32_Half    e_phnum;    // program header number 程序头表一共有多少个表项
    Elf32_Half    e_shentsize;// section header entry size 节头表的每个表项的大小
    Elf32_Half    e_shnum;    // section header number 节头表一共有多少个表项
    Elf32_Half    e_shstrndx;  // section header string table index .strtab在节头表中的索引
  };
// load的时候，先根据elf头找到程序头表，
// 程序头表的每个表项代表可执行文件中的一个节，一个表项描述了可执行文件中的节和虚拟空间中的段的映射关系
// load根据程序头表将可执行文件加载到内存中

// 程序头表的表项
/** Program header.  See [ELF1] 2-2 to 2-4.
   There are e_phnum of these, starting at file offset e_phoff
   (see [ELF1] 1-6). */
struct Elf32_Phdr
  {
    Elf32_Word p_type;  // type表示当前节是否可以加载到内存，通常可执行文件中只有两个段可以加载，分别是text和data段
    Elf32_Off  p_offset;  // 当前节在文件中的偏移量
    Elf32_Addr p_vaddr;  // 当前节加载到内存中的虚拟地址
    Elf32_Addr p_paddr;  // 
    Elf32_Word p_filesz; // 当前节在文件中的大小
    Elf32_Word p_memsz;  // 当前节加载到内存后，在内存中的大小
    Elf32_Word p_flags; // 该节加载到内存中的标志位
    Elf32_Word p_align; // 按照多少字节对齐
    // 按照memsz分配内存，按照filesz向文件中读取数据，装载入内存，它们之间的空隙用0填充
  };

/** Values for p_type.  See [ELF1] 2-3. */
#define PT_NULL    0            /**< Ignore. */
#define PT_LOAD    1            /**< Loadable segment. */
#define PT_DYNAMIC 2            /**< Dynamic linking info. */
#define PT_INTERP  3            /**< Name of dynamic loader. */
#define PT_NOTE    4            /**< Auxiliary info. */
#define PT_SHLIB   5            /**< Reserved. */
#define PT_PHDR    6            /**< Program header table. */
#define PT_STACK   0x6474e551   /**< Stack segment. */

/** Flags for p_flags.  See [ELF3] 2-3 and 2-4. */
#define PF_X 1          /**< Executable. */
#define PF_W 2          /**< Writable. */
#define PF_R 4          /**< Readable. */

static bool setup_stack (void **esp);
static bool validate_segment (const struct Elf32_Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
                          uint32_t read_bytes, uint32_t zero_bytes,
                          bool writable);

/** Loads an ELF executable from FILE_NAME into the current thread.
   Stores the executable's entry point into *EIP
   and its initial stack pointer into *ESP.
   Returns true if successful, false otherwise. */
bool
load (const char *file_name, void (**eip) (void), void **esp) 
{
  struct thread *t = thread_current ();
  struct Elf32_Ehdr ehdr;
  struct file *file = NULL;
  off_t file_ofs;
  bool success = false;
  int i;

  /* Allocate and activate page directory. */
  t->pagedir = pagedir_create ();
  if (t->pagedir == NULL) 
    goto done;
  process_activate ();

  /* Open executable file. */
  file = filesys_open (file_name);
  if (file == NULL) 
    {
      printf ("load: %s: open failed\n", file_name);
      goto done; 
    }
  // load的时候，先读取elf头，根据elf头找到程序头表，
  // 程序头表的每个表项代表可执行文件中的一个节，一个表项描述了可执行文件中的节和虚拟空间中的段的映射关系
  // load根据程序头表将可执行文件加载到内存中

  /* Read and verify executable header. */
  if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
      || memcmp (ehdr.e_ident, "\177ELF\1\1\1", 7)
      || ehdr.e_type != 2
      || ehdr.e_machine != 3
      || ehdr.e_version != 1
      || ehdr.e_phentsize != sizeof (struct Elf32_Phdr)
      || ehdr.e_phnum > 1024) 
    {
      printf ("load: %s: error loading executable\n", file_name);
      goto done; 
    }

  /* Read program headers. */
  file_ofs = ehdr.e_phoff;
  // 遍历程序头表
  for (i = 0; i < ehdr.e_phnum; i++) 
    {
      struct Elf32_Phdr phdr;

      if (file_ofs < 0 || file_ofs > file_length (file))
        goto done;
      file_seek (file, file_ofs);

      if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
        goto done;
      file_ofs += sizeof phdr;
      switch (phdr.p_type) 
        {
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
        // 只有load类型的节才能加载入内存
        case PT_LOAD:
          if (validate_segment (&phdr, file)) 
            {
              bool writable = (phdr.p_flags & PF_W) != 0;
              uint32_t file_page = phdr.p_offset & ~PGMASK;
              uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
              uint32_t page_offset = phdr.p_vaddr & PGMASK;
              uint32_t read_bytes, zero_bytes;
              if (phdr.p_filesz > 0)
                {
                  /* Normal segment.
                     Read initial part from disk and zero the rest. */
                  read_bytes = page_offset + phdr.p_filesz;
                  zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
                                - read_bytes);
                }
              else 
                {
                  /* Entirely zero.
                     Don't read anything from disk. */
                  read_bytes = 0;
                  zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
                }
              // 将可执行文件中的对应部分加载入内存
              if (!load_segment (file, file_page, (void *) mem_page,
                                 read_bytes, zero_bytes, writable))
                goto done;
            }
          else
            goto done;
          break;
        }
    }

  /* Set up stack. */
  if (!setup_stack (esp))
    goto done;

  /* Start address. */
  *eip = (void (*) (void)) ehdr.e_entry;

  success = true;

 done:
  /* We arrive here whether the load is successful or not. */
  file_close (file);
  return success;
}

/** load() helpers. */

bool install_page (void *upage, void *kpage, bool writable);

/** Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Elf32_Phdr *phdr, struct file *file) 
{
  /* p_offset and p_vaddr must have the same page offset. */
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK)) 
    return false; 

  /* p_offset must point within FILE. */
  if (phdr->p_offset > (Elf32_Off) file_length (file)) 
    return false;

  /* p_memsz must be at least as big as p_filesz. */
  if (phdr->p_memsz < phdr->p_filesz) 
    return false; 

  /* The segment must not be empty. */
  if (phdr->p_memsz == 0)
    return false;
  
  /* The virtual memory region must both start and end within the
     user address space range. */
  if (!is_user_vaddr ((void *) phdr->p_vaddr))
    return false;
  if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
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
struct vm_area_struct*
find_vm_area_struct(uint32_t upage, struct list* vm_area_list) {
  ASSERT(lock_held_by_current_thread(&thread_current()->mm->lock));
  struct list_elem *e = list_begin(vm_area_list);
  for (; e != list_end(vm_area_list); e = list_next(e)) {
    struct vm_area_struct *vas = list_entry(e, struct vm_area_struct, vm_area_list_elem);
    // 发生page fault的位置要小于end，不能等于
    if (vas->vm_start <= upage && upage < vas->vm_end) {
      return vas;
    }
  }
  return NULL;
}
static struct list_elem*
find_first_greater_than(uint32_t upage, uint32_t memsize, struct list* vm_area_list) {
  ASSERT(lock_held_by_current_thread(&thread_current()->mm->lock));
  struct list_elem *e = list_begin(vm_area_list);
  for (; e != list_end(vm_area_list); e = list_next(e)) {
    struct vm_area_struct *vas = list_entry(e, struct vm_area_struct, vm_area_list_elem);
    // 新的映射不能与之前的映射重叠
    // printf("**********upage :%d vm_start: %d vm_end :%d*************\n", upage, vas->vm_start, vas->vm_end);
    ASSERT(!(vas->vm_start < upage && upage < vas->vm_end));
    if (vas->vm_start <= upage) {
      continue;
    }
    // 映射的尾部也要小于下一段的start
    ASSERT(upage + memsize <= vas->vm_start);
    break;
  }
  return e;
}
static void
create_vm_area_struct(uint32_t upage, uint32_t zero_bytes, uint32_t read_bytes, struct file *file, off_t ofs, bool writable, bool is_stack) {
  ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT (pg_ofs ((void*)upage) == 0);
  ASSERT (ofs % PGSIZE == 0);

  lock_acquire(&thread_current()->mm->lock);
  struct list *vm_area_list = &thread_current()->mm->vm_area_list;
  struct list_elem * e = find_first_greater_than(upage, zero_bytes + read_bytes, vm_area_list);
  struct vm_area_struct *cur_vas = (struct vm_area_struct*)malloc(sizeof(struct vm_area_struct));
  // 如果是stack，把栈vm_area的起点设为上一个vm_area的终点
  if (is_stack) {
    ASSERT(!list_empty(&thread_current()->mm->vm_area_list));
    ASSERT(e == list_end(&thread_current()->mm->vm_area_list));
    struct list_elem *prev_elem = list_prev(e);
    struct vm_area_struct *prev_vas = list_entry(prev_elem, struct vm_area_struct, vm_area_list_elem);
    upage = prev_vas->vm_end + PGSIZE;
    // printf("%p\n",(void*)upage);
    // printf("%p\n",PHYS_BASE);
    zero_bytes = PHYS_BASE - upage;
    cur_vas->stack_space_top = (uint32_t)PHYS_BASE;
    ASSERT(zero_bytes % PGSIZE == 0);
  }

  // printf("upage:%p;zero_bytes:%d;read_bytes:%d;file name:%s;offset:%d;writable:%d;is_stack:%d\n",(void*)upage, zero_bytes, read_bytes, file == NULL ? NULL : file->name, ofs, writable, is_stack);

  list_insert(e, &cur_vas->vm_area_list_elem);
  cur_vas->vm_start = upage;
  cur_vas->vm_end = upage + zero_bytes + read_bytes;
  cur_vas->writable = writable;
  if (file!= NULL) strlcpy(cur_vas->name, file->name, strlen(file->name) + 1);
  else cur_vas->name[0] = 0;
  // cur_vas->inode = (file == NULL ? NULL : file->inode);
  cur_vas->read_bytes = read_bytes;
  cur_vas->zero_bytes = zero_bytes;
  cur_vas->file_pos = ofs;
  cur_vas->is_stack = is_stack; // 如果该vm_area是栈区的话，允许动态增长
  lock_init(&cur_vas->lock);
  // printf("upage:%p;zero_bytes:%d;read_bytes:%d;file name:%s;offset:%d;writable:%d;cur_vas->vm_start:%p;cur_vas->vm_end:%p\n",(void*)upage, cur_vas->zero_bytes, cur_vas->read_bytes, cur_vas->name, cur_vas->file_pos, cur_vas->writable, cur_vas->vm_start, cur_vas->vm_end);
  lock_release(&thread_current()->mm->lock);
}
/** Loads a segment starting at offset OFS in FILE at address
   UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
   memory are initialized, as follows:

        - READ_BYTES bytes at UPAGE must be read from FILE
          starting at offset OFS.

        - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.

   The pages initialized by this function must be writable by the
   user process if WRITABLE is true, read-only otherwise.

   Return true if successful, false if a memory allocation error
   or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
              uint32_t read_bytes, uint32_t zero_bytes, bool writable) 
{
  ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT (pg_ofs (upage) == 0);
  ASSERT (ofs % PGSIZE == 0);

  // 按需分页不需要在这里申请frame，只需要记录虚拟地址对应的数据页在磁盘上的位置即可
  // 随后根据不同的情况，在page_fault时做处理：
  // 如果page_read_bytes等于pagesize，在page_fault的时候从文件指定的位置读取到内存即可
  // 如果page_zero_bytes等于pagesize，在page_fault时不需要从磁盘读取数据，只需要申请一个frame将其填充全0即可
  // 如果二者都不等于pagesize，那么page_fault时先读一部分数据，然后剩下的部分填0
  #ifndef VM
  file_seek (file, ofs);
  while (read_bytes > 0 || zero_bytes > 0) 
    {
      // 一次只能读取一个page的数据，所以将page_read和page_zero切割
      /* Calculate how to fill this page.
         We will read PAGE_READ_BYTES bytes from FILE
         and zero the final PAGE_ZERO_BYTES bytes. */
      size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
      size_t page_zero_bytes = PGSIZE - page_read_bytes;

      /* Get a page of memory. */
      uint8_t *kpage = palloc_get_page (PAL_USER);
      if (kpage == NULL)
        return false;

      /* Load this page. */
      if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes)
        {
          palloc_free_page (kpage);
          return false; 
        }
      memset (kpage + page_read_bytes, 0, page_zero_bytes);

      /* Add the page to the process's address space. */
      // 把页表upage处映射到kpage对应的物理地址处
      if (!install_page (upage, kpage, writable)) 
        {
          palloc_free_page (kpage);
          return false; 
        }
      /* Advance. */
      read_bytes -= page_read_bytes;
      zero_bytes -= page_zero_bytes;
      upage += PGSIZE;
    }
  #else
  
  create_vm_area_struct((uint32_t)upage, zero_bytes, read_bytes, file, ofs, writable, false);
  #endif
  return true;
}

/** Create a minimal stack by mapping a zeroed page at the top of
   user virtual memory. */
static bool
setup_stack (void **esp) 
{
  bool success = false;
  void *upage = ((void*) PHYS_BASE) - PGSIZE;

  #ifndef VM
  uint8_t *kpage;
  kpage = palloc_get_page (PAL_USER | PAL_ZERO);
  if (kpage != NULL) 
    {
      success = install_page (upage, kpage, true);
      if (success)
        *esp = PHYS_BASE;
      else
        palloc_free_page (kpage);
    }
  #else
  // upage = (void*)((uint32_t)PHYS_BASE - 10 * PGSIZE);
  create_vm_area_struct((uint32_t)upage, PGSIZE, 0, NULL, 0, true, true);
  *esp = PHYS_BASE; 
  success = true;
  #endif

  return success;
}

/** Adds a mapping from user virtual address UPAGE to kernel
   virtual address KPAGE to the page table.
   If WRITABLE is true, the user process may modify the page;
   otherwise, it is read-only.
   UPAGE must not already be mapped.
   KPAGE should probably be a page obtained from the user pool
   with palloc_get_page().
   Returns true on success, false if UPAGE is already mapped or
   if memory allocation fails. */
bool
install_page (void *upage, void *kpage, bool writable)
{
  struct thread *t = thread_current ();
  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
  // 如果upage已经映射了，或者设置upage失败了，则返回false
  if (pagedir_get_page (t->pagedir, upage) != NULL
          || !pagedir_set_page (t->pagedir, upage, kpage, writable))
      return false;
  // 映射成功后更新相应的frame的字段
  map_frame_to(upage, kpage);
  return true;
}
