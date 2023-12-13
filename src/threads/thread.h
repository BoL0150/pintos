#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

#include <debug.h>
#include <list.h>
#include <stdint.h>
#include "fixed-point.h"
#include "filesys/file.h"
#include "synch.h"
/** States in a thread's life cycle. */
enum thread_status
  {
    THREAD_RUNNING,     /**< Running thread. */
    THREAD_READY,       /**< Not running but ready to run. */
    THREAD_BLOCKED,     /**< Waiting for an event to trigger. */
    THREAD_DYING        /**< About to be destroyed. */
  };
/** Thread identifier type.
   You can redefine this to whatever type you like. */
typedef int tid_t;
#define TID_ERROR ((tid_t) -1)          /**< Error value for tid_t. */

/** Thread priorities. */
#define PRI_MIN 0                       /**< Lowest priority. */
#define PRI_DEFAULT 31                  /**< Default priority. */
#define PRI_MAX 63                      /**< Highest priority. */
#define PRI_QUEUE_NUM (PRI_MAX - PRI_MIN + 1)
/** A kernel thread or user process.

   Each thread structure is stored in its own 4 kB page.  The
   thread structure itself sits at the very bottom of the page
   (at offset 0).  The rest of the page is reserved for the
   thread's kernel stack, which grows downward from the top of
   the page (at offset 4 kB).  Here's an illustration:

        4 kB +---------------------------------+
             |          kernel stack           |
             |                |                |
             |                |                |
             |                V                |
             |         grows downward          |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             +---------------------------------+
             |              magic              |
             |                :                |
             |                :                |
             |               name              |
             |              status             |
        0 kB +---------------------------------+

   The upshot of this is twofold:

      1. First, `struct thread' must not be allowed to grow too
         big.  If it does, then there will not be enough room for
         the kernel stack.  Our base `struct thread' is only a
         few bytes in size.  It probably should stay well under 1
         kB.

      2. Second, kernel stacks must not be allowed to grow too
         large.  If a stack overflows, it will corrupt the thread
         state.  Thus, kernel functions should not allocate large
         structures or arrays as non-static local variables.  Use
         dynamic allocation with malloc() or palloc_get_page()
         instead.

   The first symptom of either of these problems will probably be
   an assertion failure in thread_current(), which checks that
   the `magic' member of the running thread's `struct thread' is
   set to THREAD_MAGIC.  Stack overflow will normally change this
   value, triggering the assertion. */
/** The `elem' member has a dual purpose.  It can be an element in
   the run queue (thread.c), or it can be an element in a
   semaphore wait list (synch.c).  It can be used these two ways
   only because they are mutually exclusive: only a thread in the
   ready state is on the run queue, whereas only a thread in the
   blocked state is on a semaphore wait list. */
struct thread
  {
    /* Owned by thread.c. */
    tid_t tid;                          /**< Thread identifier. */
    enum thread_status status;          /**< Thread state. */
    int exit_state;                     /**< exit state to be returned to parent's wait*/
    struct thread *parent;              /**< parent process*/
    char name[16];                      /**< Name (for debugging purposes). */
    uint8_t *stack;                     /**< Saved stack pointer. */
    int priority;                       /**< Priority. */
    struct list lock_list;              /**< 当前线程持有的锁*/
    struct list child_list;             /**< 当前线程exec的所有子线程，元素中记录了对应线程的tid和退出状态*/
    struct thread * blocked_by;
    int true_pri;
    int nice;
    fp32_t recent_cpu;                 /** fixed-point type */
    struct list_elem allelem;           /**< List element for all threads list. */
    /* Shared between thread.c and synch.c. */
    struct list_elem elem;              /**< List element for ready_list and blocked list in semaphores 
                                             线程要么就在ready_list中，要么就在semaphores的waiterlist中*/
    struct list_elem pri_list_elem;     /**< List element for priority queue list*/
    struct file *ofile[32];          /**< 进程打开文件表*/
    struct mm_struct *mm;
#ifdef USERPROG
    /* Owned by userprog/process.c. */
    uint32_t *pagedir;                  /**< Page directory. */
#endif

    /* Owned by thread.c. */
    unsigned magic;                     /**< Detects stack overflow. */
  };
// 为sleep功能专门封装了一个struct
typedef struct{
  int64_t remain_time;
  struct thread * t;
  struct list_elem elem;
}sleep_thread;

struct thread_exit_state{
   tid_t tid;
   int exit_state;
   struct semaphore sema;
   struct list_elem child_list_elem;
};
struct mm_struct {
   struct list vm_area_list;
   struct lock lock;
};
struct vm_area_struct {
   struct list_elem vm_area_list_elem;
   char name[16];             
   uint32_t vm_start;
   uint32_t vm_end;
   bool writable;
   uint32_t read_bytes;
   uint32_t zero_bytes; 
   off_t file_pos;
   struct lock lock;
   // 用于栈动态增长
   bool is_stack;
   bool is_mmap;
   int fd;
};
struct thread * find_max_pri_thread_from_pri_queue (void);
void increase_recent_cpu(void);
void update_recent_cpu(void);
void update_load_avg(void);
bool update_mlfqs_priority(struct thread* t);
bool update_all_threads_mlfqs_priority(void);

struct thread *
find_max_pri_thread (struct list * l);
bool
add_to_ready_list (struct thread *t);
void 
add_sleep_thread (struct thread *t, int64_t sleep_time);
void update_sleep_list (void);
/** If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
extern bool thread_mlfqs;
/**
 * fixed-point type
*/
extern fp32_t load_avg;

void thread_init (void);
void thread_start (void);

void thread_tick (void);
void thread_print_stats (void);

typedef void thread_func (void *aux);
tid_t thread_create (const char *name, int priority, thread_func *, void *);

void thread_block (void);
bool thread_unblock (struct thread *);

struct thread *thread_current (void);
tid_t thread_tid (void);
const char *thread_name (void);

void thread_exit (void) NO_RETURN;
void thread_yield (void);

/** Performs some operation on thread t, given auxiliary data AUX. */
typedef void thread_action_func (struct thread *t, void *aux);
void thread_foreach (thread_action_func *, void *);

int thread_get_priority (void);
void thread_set_priority (int);

int thread_get_nice (void);
void thread_set_nice (int);
int thread_get_recent_cpu (void);
int thread_get_load_avg (void);

#endif /**< threads/thread.h */
