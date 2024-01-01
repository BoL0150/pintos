#include "userprog/syscall.h"
#include "userprog/pagedir.h"
#include "filesys/filesys.h"
#include "threads/palloc.h"
#include "lib/string.h"
#include "userprog/process.h"
#include "filesys/inode.h"
#include "round.h"
#include "userprog/exception.h"
#include "threads/vaddr.h"
#include <inttypes.h>
#include <stdio.h>
#include "userprog/gdt.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "userprog/process.h"
/** Number of page faults processed. */
static long long page_fault_cnt;

static void kill (struct intr_frame *);
static void page_fault (struct intr_frame *);
extern struct lock filesys_lock;
/** Registers handlers for interrupts that can be caused by user
   programs.

   In a real Unix-like OS, most of these interrupts would be
   passed along to the user process in the form of signals, as
   described in [SV-386] 3-24 and 3-25, but we don't implement
   signals.  Instead, we'll make them simply kill the user
   process.

   Page faults are an exception.  Here they are treated the same
   way as other exceptions, but this will need to change to
   implement virtual memory.

   Refer to [IA32-v3a] section 5.15 "Exception and Interrupt
   Reference" for a description of each of these exceptions. */
void
exception_init (void) 
{
  /* These exceptions can be raised explicitly by a user program,
     e.g. via the INT, INT3, INTO, and BOUND instructions.  Thus,
     we set DPL==3, meaning that user programs are allowed to
     invoke them via these instructions. */
  intr_register_int (3, 3, INTR_ON, kill, "#BP Breakpoint Exception");
  intr_register_int (4, 3, INTR_ON, kill, "#OF Overflow Exception");
  intr_register_int (5, 3, INTR_ON, kill,
                     "#BR BOUND Range Exceeded Exception");

  /* These exceptions have DPL==0, preventing user processes from
     invoking them via the INT instruction.  They can still be
     caused indirectly, e.g. #DE can be caused by dividing by
     0.  */
  intr_register_int (0, 0, INTR_ON, kill, "#DE Divide Error");
  intr_register_int (1, 0, INTR_ON, kill, "#DB Debug Exception");
  intr_register_int (6, 0, INTR_ON, kill, "#UD Invalid Opcode Exception");
  intr_register_int (7, 0, INTR_ON, kill,
                     "#NM Device Not Available Exception");
  intr_register_int (11, 0, INTR_ON, kill, "#NP Segment Not Present");
  intr_register_int (12, 0, INTR_ON, kill, "#SS Stack Fault Exception");
  intr_register_int (13, 0, INTR_ON, kill, "#GP General Protection Exception");
  intr_register_int (16, 0, INTR_ON, kill, "#MF x87 FPU Floating-Point Error");
  intr_register_int (19, 0, INTR_ON, kill,
                     "#XF SIMD Floating-Point Exception");

  /* Most exceptions can be handled with interrupts turned on.
     We need to disable interrupts for page faults because the
     fault address is stored in CR2 and needs to be preserved. */
  intr_register_int (14, 0, INTR_OFF, page_fault, "#PF Page-Fault Exception");
}

/** Prints exception statistics. */
void
exception_print_stats (void) 
{
  printf ("Exception: %lld page faults\n", page_fault_cnt);
}

/** Handler for an exception (probably) caused by a user process. */
static void
kill (struct intr_frame *f) 
{
  /* This interrupt is one (probably) caused by a user process.
     For example, the process might have tried to access unmapped
     virtual memory (a page fault).  For now, we simply kill the
     user process.  Later, we'll want to handle page faults in
     the kernel.  Real Unix-like operating systems pass most
     exceptions back to the process via signals, but we don't
     implement them. */

   // 被异常kill的线程exit_state设为-1
   thread_current()->exit_state = -1;
   printf("%s: exit(%d)\n",thread_current()->name, thread_current()->exit_state);
  /* The interrupt frame's code segment value tells us where the
     exception originated. */
  switch (f->cs)
    {
    case SEL_UCSEG:
      /* User's code segment, so it's a user exception, as we
         expected.  Kill the user process.  */
      printf ("%s: dying due to interrupt %#04x (%s).\n",
              thread_name (), f->vec_no, intr_name (f->vec_no));
      intr_dump_frame (f);
      thread_exit (); 

    case SEL_KCSEG:
      /* Kernel's code segment, which indicates a kernel bug.
         Kernel code shouldn't throw exceptions.  (Page faults
         may cause kernel exceptions--but they shouldn't arrive
         here.)  Panic the kernel to make the point.  */
      intr_dump_frame (f);
      PANIC ("Kernel bug - unexpected interrupt in kernel"); 

    default:
      /* Some other code segment?  Shouldn't happen.  Panic the
         kernel. */
      printf ("Interrupt %#04x (%s) in unknown segment %04x\n",
             f->vec_no, intr_name (f->vec_no), f->cs);
      thread_exit ();
    }
}
static void
read_data_from_file(struct intr_frame* f, struct vm_area_struct *vas, void *kpage, uint32_t read_offset, uint32_t page_read_bytes) {
   // 栈区的file为空
   if (vas->file == NULL) return;
   struct file *file = vas->file;
   file_seek(file, vas->file_pos + read_offset);
   if (file_read(file, kpage, page_read_bytes) != page_read_bytes) {
      palloc_free_page(kpage);
      lock_release(&vas->lock);
      lock_release(&filesys_lock);
      kill(f);
   }
}
// 鉴定为傻逼做法，要是复制结构体的时候数据被修改了怎么办？
// static struct vm_area_struct * vas_copy(struct vm_area_struct *vas) {
//    ASSERT(vas != NULL);
//    struct vm_area_struct *new_vas = (struct vm_area_struct *)malloc(sizeof(struct vm_area_struct));
//    memcpy((void*)new_vas, (void*)vas, sizeof(struct vm_area_struct));
//    return new_vas;
// }
static void
demand_paging(struct intr_frame* f, void *fault_addr) {
   lock_acquire(&thread_current()->mm->lock);
   // printf("************exception**************\n");
   // printf("%p\n", fault_addr);
   struct vm_area_struct *vas = find_vm_area_struct((uint32_t)fault_addr, thread_current()->mm);
   // 按需分页是由用户触发的异常，如果用户传入的地址不存在，那么就exit(-1)
   if (vas == NULL) {
      lock_release(&thread_current()->mm->lock);
      exit_from_user_process(-1);
   }
   lock_acquire(&vas->lock);
   lock_release(&thread_current()->mm->lock);
   // printf("zero_bytes:%d;read_bytes:%d;file name:%s;offset:%d;writable:%d;vas->vm_start:%p;vas->vm_end:%p;is_stack:%d\n",vas->zero_bytes, vas->read_bytes, vas->file->name, vas->file_pos, vas->writable, vas->vm_start, vas->vm_end, vas->is_stack);
   // 栈的增长范围只允许在esp下面一个page内,新的栈要和旧的栈连续
   if (vas->is_stack) {
      // 由于是在内核中出现的pagefault，所以intr_frame被替换，处理pagefault时使用的esp应该是用户的esp
      if (thread_current()->user_esp != NULL) {
         f->esp = thread_current()->user_esp;
      }
      // esp要低于PHYS_BASE
      uint32_t esp = f->esp > PHYS_BASE ? PHYS_BASE : f->esp;
      // esp要在vm_area区间内，不能随便修改
      if ((esp > vas->vm_end || esp < vas->vm_start) || (uint32_t)fault_addr + PGSIZE <= esp) {
         lock_release(&vas->lock);
         exit_from_user_process(-1); 
      } 
   }
   uint32_t upage = ROUND_DOWN((uint32_t)fault_addr, PGSIZE);
   uint32_t read_offset = upage - vas->vm_start;
   ASSERT(vas->vm_start <= upage && upage + PGSIZE <= vas->vm_end);
   uint32_t read_end = vas->vm_start + vas->read_bytes;
   if (read_end < upage) read_end = upage;
   size_t page_read_bytes = ((read_end - upage) < PGSIZE ? (read_end - upage) : PGSIZE);
   size_t page_zero_bytes = PGSIZE - page_read_bytes;
   void* kpage = palloc_get_page(PAL_USER);
   ASSERT(kpage != NULL);
   // 锁的范围太小了，需要在函数每一次提前返回时都释放锁，非常容易漏写；
   // 如果发现有内存泄漏或者重复拿锁，那么很有可能是你在某一个函数中提前返回时忘了释放资源
   lock_acquire(&filesys_lock);
   read_data_from_file(f, vas, kpage, read_offset, page_read_bytes);
   lock_release(&filesys_lock);

   memset (kpage + page_read_bytes, 0, page_zero_bytes);
   if (!install_page((void*)upage, kpage, vas->writable)) {
      palloc_free_page(kpage);
      lock_release(&vas->lock);
      kill(f);
   }
   // printf("read page, vaddr %p, frame_no %d from file %s\n", upage, get_kpage_no(kpage), vas->name);
   lock_release(&vas->lock);
}
/** Page fault handler.  This is a skeleton that must be filled in
   to implement virtual memory.  Some solutions to project 2 may
   also require modifying this code.

   At entry, the address that faulted is in CR2 (Control Register
   2) and information about the fault, formatted as described in
   the PF_* macros in exception.h, is in F's error_code member.  The
   example code here shows how to parse that information.  You
   can find more information about both of these in the
   description of "Interrupt 14--Page Fault Exception (#PF)" in
   [IA32-v3a] section 5.15 "Exception and Interrupt Reference". */
static void
page_fault (struct intr_frame *f) 
{
  bool not_present;  /**< True: not-present page, false: writing r/o page. */
  bool write;        /**< True: access was write, false: access was read. */
  bool user;         /**< True: access by user, false: access by kernel. */
  void *fault_addr;  /**< Fault address. */

  /* Obtain faulting address, the virtual address that was
     accessed to cause the fault.  It may point to code or to
     data.  It is not necessarily the address of the instruction
     that caused the fault (that's f->eip).
     See [IA32-v2a] "MOV--Move to/from Control Registers" and
     [IA32-v3a] 5.15 "Interrupt 14--Page Fault Exception
     (#PF)". */
  asm ("movl %%cr2, %0" : "=r" (fault_addr));

  /* Turn interrupts back on (they were only off so that we could
     be assured of reading CR2 before it changed). */
  intr_enable ();

  /* Count page faults. */
  page_fault_cnt++;

  /* Determine cause. */
  not_present = (f->error_code & PF_P) == 0;
  write = (f->error_code & PF_W) != 0;
  user = (f->error_code & PF_U) != 0;

  // printf("exception esp:%p\n",f->esp);
  // printf ("Page fault at %p: %s error %s page in %s context.\n",
  //         fault_addr,
  //         not_present ? "not present" : "rights violation",
  //         write ? "writing" : "reading",
  //         user ? "user" : "kernel");
  // 不允许在内核中发生pagefault，所以这里一定不可能有filesys_lock
   // ASSERT(!lock_held_by_current_thread(&filesys_lock));
   if (not_present) {
      ASSERT(!pagedir_is_present(thread_current()->pagedir, fault_addr));

      if (is_data_on_swap_partition(thread_current()->pagedir, fault_addr)) {
         // printf("**********read data from swap partition**********\n");
         void *kpage = palloc_get_page(PAL_USER);
         void *upage = (void*)ROUND_DOWN((uint32_t)fault_addr, PGSIZE);
         install_page((void*)upage, kpage, pagedir_is_writable(thread_current()->pagedir, fault_addr));
         // 从交换分区中重新fetch数据、install PTE后，不需要取消新的PTE的swap flag（因为新的pte的swap flag本来就是0），
         // 但是需要将pte的脏位置为true(install时默认的脏位是false)
         // clear_pte_swap_flag(thread_create()->pagedir, fault_addr);
         pagedir_set_dirty(thread_current()->pagedir, fault_addr, true);
         // printf("read page, vaddr %p, frame_no %d from swap slot %d\n", upage, get_kpage_no(kpage), swap_slot_idx);
      }
      else {
         // printf("**********read data from file sys**********\n");
         // 不要在这里拿锁，因为demand_paging里面出错会调用kill，执行流被打断，锁不会被释放
         // if (!lock_held_by_current_thread(&filesys_lock)) lock_acquire(&filesys_lock);
         demand_paging(f, fault_addr);
         // if (lock_held_by_current_thread(&filesys_lock)) lock_release(&filesys_lock);
      }
      // demand_paging(f, fault_addr);
      return; // 分配成功，从中断handler中返回
   } else {
      exit_from_user_process(-1);
   }

  kill (f);
}

