#ifndef THREADS_SYNCH_H
#define THREADS_SYNCH_H

#include <list.h>
#include <stdbool.h>

/** A counting semaphore. */
struct semaphore 
  {
    unsigned value;             /**< Current value. */
    struct list waiters;        /**< List of waiting threads. */
  };

/** One semaphore in a list. */
// 在pintos的条件变量中，为每个线程维护一个信号量，线程的睡眠和唤醒都是通过信号量来进行的
struct semaphore_elem 
  {
    struct list_elem elem;              /**< List element. */
    struct semaphore semaphore;         /**< This semaphore. */
  };

/** Lock. */
struct lock 
  {
    struct list_elem elem;
    struct thread *holder;      /**< Thread holding lock (for debugging). */
    struct semaphore semaphore; /**< Binary semaphore controlling access. */
  };

void update_upstream_thread_pri (struct thread *t, int priority);
struct thread * get_max_pri_thread (struct thread * a, struct thread * b);
struct thread * find_max_pri_thread_blocked_by (struct thread * cur_thread);
struct thread * find_max_pri_thread_among_locks (struct list * lock_list);
struct semaphore * find_and_rm_max_pri_sema (struct list * l);
void pri_inverse_sema_down (struct lock *lock);
bool pri_inverse_sema_up (struct lock *lock);

void sema_init (struct semaphore *, unsigned value);
void sema_down (struct semaphore *);
bool sema_try_down (struct semaphore *);
void sema_up (struct semaphore *);
void sema_self_test (void);


void lock_init (struct lock *);
void lock_acquire (struct lock *);
bool lock_try_acquire (struct lock *);
void lock_release (struct lock *);
bool lock_held_by_current_thread (const struct lock *);

/** Condition variable. 
 * 此条件变量的list中连接的不是waiting的线程，而是先给每个线程创建一个sema，把sema连接到条件变量的
 * waiters list上。然后唤醒时是从list中选取sema，然后唤醒这个sema对应的线程
*/
struct condition 
  {
    struct list waiters;        /**< List of waiting threads. */
  };

void cond_init (struct condition *);
void cond_wait (struct condition *, struct lock *);
void cond_signal (struct condition *, struct lock *);
void cond_broadcast (struct condition *, struct lock *);

/** Optimization barrier.

   The compiler will not reorder operations across an
   optimization barrier.  See "Optimization Barriers" in the
   reference guide for more information.*/
#define barrier() asm volatile ("" : : : "memory")

#endif /**< threads/synch.h */
