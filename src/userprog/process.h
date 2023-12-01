#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"
#define MAXARG 32
tid_t process_execute (const char *file_name);
int process_wait (tid_t);
void exit_from_user_process(int exit_state);
void process_exit (void);
void process_activate (void);

#endif /**< userprog/process.h */
