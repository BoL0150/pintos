#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"
#define MAXARG 32
tid_t process_execute (const char *file_name);
int process_wait (tid_t);
void exit_from_user_process(int exit_state);
void process_exit (void);
void process_activate (void);
int create_mmap_vm_area_struct(int fd, void *addr);
struct vm_area_struct* find_vm_area_struct(uint32_t upage, struct mm_struct* mm);
bool install_page (void *upage, void *kpage, bool writable);
struct list_elem* find_mmap_vm_area_struct(int mapping);
void destory_mmap_vas(struct vm_area_struct* vas);

#endif /**< userprog/process.h */
