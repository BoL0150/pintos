/** This file is derived from source code for the Nachos
   instructional operating system.  The Nachos copyright notice
   is reproduced in full below. */

/** Copyright (c) 1992-1996 The Regents of the University of California.
   All rights reserved.

   Permission to use, copy, modify, and distribute this software
   and its documentation for any purpose, without fee, and
   without written agreement is hereby granted, provided that the
   above copyright notice and the following two paragraphs appear
   in all copies of this software.

   IN NO EVENT SHALL THE UNIVERSITY OF CALIFORNIA BE LIABLE TO
   ANY PARTY FOR DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR
   CONSEQUENTIAL DAMAGES ARISING OUT OF THE USE OF THIS SOFTWARE
   AND ITS DOCUMENTATION, EVEN IF THE UNIVERSITY OF CALIFORNIA
   HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

   THE UNIVERSITY OF CALIFORNIA SPECIFICALLY DISCLAIMS ANY
   WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
   WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
   PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS ON AN "AS IS"
   BASIS, AND THE UNIVERSITY OF CALIFORNIA HAS NO OBLIGATION TO
   PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR
   MODIFICATIONS.
*/

#include "threads/synch.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
struct semaphore *
find_and_rm_max_pri_sema (struct list * cond_waiters) {
  ASSERT (cond_waiters != NULL);
  ASSERT (intr_get_level() == INTR_OFF);
  struct list_elem * e;
  int64_t max_pri = -1;
  struct semaphore_elem * max_pri_sema_elem;
  for (e = list_begin(cond_waiters); e != list_end(cond_waiters); e = list_next(e)) {
    struct semaphore_elem *sema = list_entry (e, struct semaphore_elem, elem);
    struct list * threads = &sema->semaphore.waiters;
    ASSERT (list_size (threads) == 1);
    struct thread * cur_thread = list_entry (list_begin(threads), struct thread, elem);
    if (cur_thread->priority > max_pri) {
      max_pri = cur_thread->priority;
      max_pri_sema_elem = sema;
    }
  }
  list_remove (&max_pri_sema_elem->elem);
  return &max_pri_sema_elem->semaphore;
}
struct thread *
get_max_pri_thread (struct thread * a, struct thread * b) {
  if (a == NULL && b != NULL) return b;
  if (a != NULL && b == NULL) return a;
  if (a->priority > b->priority) return a;
  return b;
}
struct thread *
find_max_pri_thread_blocked_by (struct thread * cur_thread) {
  ASSERT (cur_thread != NULL);
  ASSERT (intr_get_level() == INTR_OFF);
  struct list * lock_list = &cur_thread->lock_list;
  ASSERT (lock_list != NULL);
  if (list_empty(lock_list)) return NULL;

  struct thread *max_pri_thread = NULL;

  for (struct list_elem * e = list_begin(lock_list); e != list_end(lock_list); e = list_next(e)) {
    struct lock *lock = list_entry(e, struct lock, elem);
    struct list *blocked_threads = &lock->semaphore.waiters;
    for (struct list_elem * ee = list_begin(blocked_threads); ee != list_end(blocked_threads); ee = list_next(ee)) {
      struct thread * t = list_entry(ee, struct thread, elem);
      struct thread * temp = find_max_pri_thread_blocked_by (t);
      max_pri_thread = get_max_pri_thread(get_max_pri_thread(t, temp), max_pri_thread);
    }
  }
  return max_pri_thread;
}
// 更新上游的线程的优先级
void update_upstream_thread_pri (struct thread *t, int priority) {
  if (t == NULL || t->priority > priority) return;
  t->priority = priority;
  update_upstream_thread_pri(t->blocked_by, priority);
}
void
pri_inverse_sema_down (struct lock *lock) 
{
  enum intr_level old_level;

  ASSERT (lock != NULL);
  ASSERT (!intr_context ());
  struct semaphore *sema = &lock->semaphore;
  struct thread * lock_holder = lock->holder;
  old_level = intr_disable ();

  while (sema->value == 0) 
    {
      ASSERT (lock_holder != NULL);
      list_push_back (&sema->waiters, &thread_current ()->elem);
      thread_current()->blocked_by = lock_holder;
      ASSERT (!list_empty(&lock_holder->lock_list));
      // struct thread *max_pri_thread = find_max_pri_thread_among_locks(&lock_holder->lock_list);
      struct thread *max_pri_thread = find_max_pri_thread_blocked_by(lock_holder);
      ASSERT (max_pri_thread != NULL);
      update_upstream_thread_pri (lock_holder, max_pri_thread->priority);
      // lock_holder->priority = max (lock_holder->true_pri, max_pri_thread->priority);
      thread_block ();
    }
  list_push_back(&thread_current()->lock_list, &lock->elem);
  sema->value--;
  intr_set_level (old_level);
}


void
pri_inverse_sema_up (struct lock *lock) 
{
  enum intr_level old_level;
  bool yield = false;
  ASSERT (lock != NULL);
  struct semaphore * sema = &lock->semaphore;

  old_level = intr_disable ();

  struct thread * wakeup_thread = NULL;
  if (!list_empty (&sema->waiters)) {
    wakeup_thread = find_max_pri_thread(&sema->waiters);
    list_remove (&wakeup_thread->elem);
    wakeup_thread->blocked_by = NULL;
  }
  sema->value++;
  // 当前线程释放锁，就把锁从当前线程的lock_list中移除
  list_remove (&lock->elem);
  // 如果当前线程没有任何阻塞线程，则把优先级恢复成真实的优先级
  struct thread * max_pri_thread = find_max_pri_thread_blocked_by(thread_current());
  if (max_pri_thread == NULL)
    thread_current()->priority = thread_current()->true_pri;
  else 
    thread_current()->priority = max(max_pri_thread->priority, thread_current()->true_pri);
  // wakeup一个线程后，将当前线程的优先级更新后，再将该线程唤醒，因为在unblock中有比较当前线程优先级和唤醒线程优先级的操作
  if (wakeup_thread != NULL) yield = thread_unblock (wakeup_thread);

  intr_set_level (old_level);
  // 原子性结束，可以切换线程
  if (yield) {
    if (intr_context ()) intr_yield_on_return ();
    else thread_yield ();
  }
}
/** Initializes semaphore SEMA to VALUE.  A semaphore is a
   nonnegative integer along with two atomic operators for
   manipulating it:

   - down or "P": wait for the value to become positive, then
     decrement it.

   - up or "V": increment the value (and wake up one waiting
     thread, if any). */
void
sema_init (struct semaphore *sema, unsigned value) 
{
  ASSERT (sema != NULL);

  sema->value = value;
  list_init (&sema->waiters);
}

/** Down or "P" operation on a semaphore.  Waits for SEMA's value
   to become positive and then atomically decrements it.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but if it sleeps then the next scheduled
   thread will probably turn interrupts back on. */
void
sema_down (struct semaphore *sema) 
{
  enum intr_level old_level;

  ASSERT (sema != NULL);
  ASSERT (!intr_context ());

  old_level = intr_disable ();
  while (sema->value == 0) 
    {
      list_push_back (&sema->waiters, &thread_current ()->elem);
      thread_block ();
    }
  sema->value--;
  intr_set_level (old_level);
}

/** Down or "P" operation on a semaphore, but only if the
   semaphore is not already 0.  Returns true if the semaphore is
   decremented, false otherwise.

   This function may be called from an interrupt handler. */
bool
sema_try_down (struct semaphore *sema) 
{
  enum intr_level old_level;
  bool success;

  ASSERT (sema != NULL);

  old_level = intr_disable ();
  if (sema->value > 0) 
    {
      sema->value--;
      success = true; 
    }
  else
    success = false;
  intr_set_level (old_level);

  return success;
}

/** Up or "V" operation on a semaphore.  Increments SEMA's value
   and wakes up one thread of those waiting for SEMA, if any.

   This function may be called from an interrupt handler. */
void
sema_up (struct semaphore *sema) 
{
  enum intr_level old_level;
  bool yield = false;
  ASSERT (sema != NULL);

  old_level = intr_disable ();
  
  if (!list_empty (&sema->waiters)) {
    struct thread * max_pri_thread = find_max_pri_thread(&sema->waiters);
    list_remove (&max_pri_thread->elem);
    yield = thread_unblock (max_pri_thread);
  }
  sema->value++;
  intr_set_level (old_level);
  // 原子性结束，可以切换线程
  if (yield) {
    if (intr_context ()) intr_yield_on_return ();
    else thread_yield ();
  }
}

static void sema_test_helper (void *sema_);

/** Self-test for semaphores that makes control "ping-pong"
   between a pair of threads.  Insert calls to printf() to see
   what's going on. */
void
sema_self_test (void) 
{
  struct semaphore sema[2];
  int i;

  printf ("Testing semaphores...");
  sema_init (&sema[0], 0);
  sema_init (&sema[1], 0);
  thread_create ("sema-test", PRI_DEFAULT, sema_test_helper, &sema);
  for (i = 0; i < 10; i++) 
    {
      sema_up (&sema[0]);
      sema_down (&sema[1]);
    }
  printf ("done.\n");
}

/** Thread function used by sema_self_test(). */
static void
sema_test_helper (void *sema_) 
{
  struct semaphore *sema = sema_;
  int i;

  for (i = 0; i < 10; i++) 
    {
      sema_down (&sema[0]);
      sema_up (&sema[1]);
    }
}

/** Initializes LOCK.  A lock can be held by at most a single
   thread at any given time.  Our locks are not "recursive", that
   is, it is an error for the thread currently holding a lock to
   try to acquire that lock.

   A lock is a specialization of a semaphore with an initial
   value of 1.  The difference between a lock and such a
   semaphore is twofold.  First, a semaphore can have a value
   greater than 1, but a lock can only be owned by a single
   thread at a time.  Second, a semaphore does not have an owner,
   meaning that one thread can "down" the semaphore and then
   another one "up" it, but with a lock the same thread must both
   acquire and release it.  When these restrictions prove
   onerous, it's a good sign that a semaphore should be used,
   instead of a lock. */
void
lock_init (struct lock *lock)
{
  ASSERT (lock != NULL);

  lock->holder = NULL;
  sema_init (&lock->semaphore, 1);
}

/** Acquires LOCK, sleeping until it becomes available if
   necessary.  The lock must not already be held by the current
   thread.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but interrupts will be turned back on if
   we need to sleep. */
void
lock_acquire (struct lock *lock)
{
  ASSERT (lock != NULL);
  ASSERT (!intr_context ());
  ASSERT (!lock_held_by_current_thread (lock));
  if (!thread_mlfqs) pri_inverse_sema_down(lock);
  else sema_down(&lock->semaphore);
  lock->holder = thread_current ();
}

/** Tries to acquires LOCK and returns true if successful or false
   on failure.  The lock must not already be held by the current
   thread.

   This function will not sleep, so it may be called within an
   interrupt handler. */
bool
lock_try_acquire (struct lock *lock)
{
  bool success;

  ASSERT (lock != NULL);
  ASSERT (!lock_held_by_current_thread (lock));

  success = sema_try_down (&lock->semaphore);
  if (success)
    lock->holder = thread_current ();
  return success;
}
/** Releases LOCK, which must be owned by the current thread.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to release a lock within an interrupt
   handler. */
void
lock_release (struct lock *lock) 
{
  ASSERT (lock != NULL);
  ASSERT (lock_held_by_current_thread (lock));

  lock->holder = NULL;
  if(!thread_mlfqs) pri_inverse_sema_up (lock);
  else sema_up(&lock->semaphore);
}

/** Returns true if the current thread holds LOCK, false
   otherwise.  (Note that testing whether some other thread holds
   a lock would be racy.) */
bool
lock_held_by_current_thread (const struct lock *lock) 
{
  ASSERT (lock != NULL);

  return lock->holder == thread_current ();
}


/** Initializes condition variable COND.  A condition variable
   allows one piece of code to signal a condition and cooperating
   code to receive the signal and act upon it. */
void
cond_init (struct condition *cond)
{
  ASSERT (cond != NULL);

  list_init (&cond->waiters);
}

/** Atomically releases LOCK and waits for COND to be signaled by
   some other piece of code.  After COND is signaled, LOCK is
   reacquired before returning.  LOCK must be held before calling
   this function.

   The monitor implemented by this function is "Mesa" style, not
   "Hoare" style, that is, sending and receiving a signal are not
   an atomic operation.  Thus, typically the caller must recheck
   the condition after the wait completes and, if necessary, wait
   again.

   A given condition variable is associated with only a single
   lock, but one lock may be associated with any number of
   condition variables.  That is, there is a one-to-many mapping
   from locks to condition variables.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but interrupts will be turned back on if
   we need to sleep. */
  // 在pintos的条件变量中，为每个线程维护一个信号量，线程的睡眠和唤醒都是通过信号量来进行的
void
cond_wait (struct condition *cond, struct lock *lock) 
{
  struct semaphore_elem waiter;

  ASSERT (cond != NULL);
  ASSERT (lock != NULL);
  ASSERT (!intr_context ());
  ASSERT (lock_held_by_current_thread (lock));
  
  sema_init (&waiter.semaphore, 0);
  list_push_back (&cond->waiters, &waiter.elem);

  list_size (&cond->waiters);
  list_size (&list_entry (list_begin (&cond->waiters), struct semaphore_elem, elem)->semaphore.waiters);

  lock_release (lock);
  sema_down (&waiter.semaphore);
  lock_acquire (lock);
}

/** If any threads are waiting on COND (protected by LOCK), then
   this function signals one of them to wake up from its wait.
   LOCK must be held before calling this function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to signal a condition variable within an
   interrupt handler. */
void
cond_signal (struct condition *cond, struct lock *lock UNUSED) 
{
  ASSERT (cond != NULL);
  ASSERT (lock != NULL);
  ASSERT (!intr_context ());
  ASSERT (lock_held_by_current_thread (lock));

  if (!list_empty (&cond->waiters)) {
    enum intr_level old_level = intr_disable ();
    sema_up (find_and_rm_max_pri_sema (&cond->waiters));
    intr_set_level (old_level);
  }
}

/** Wakes up all threads, if any, waiting on COND (protected by
   LOCK).  LOCK must be held before calling this function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to signal a condition variable within an
   interrupt handler. */
void
cond_broadcast (struct condition *cond, struct lock *lock) 
{
  ASSERT (cond != NULL);
  ASSERT (lock != NULL);

  while (!list_empty (&cond->waiters))
    cond_signal (cond, lock);
}
