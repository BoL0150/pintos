#ifndef FILESYS_FILE_H
#define FILESYS_FILE_H

#include "filesys/off_t.h"
#include <debug.h>
#include "threads/thread.h"
#define FDNUM 32                        /**< 一个进程中的文件描述符个数*/
#define OFILENUM 100
struct inode;

/** An open file. */
struct file 
  {
    // char name[16];              /**< Name (for debugging purposes). */
    struct inode *inode;        /**< File's inode. */
    off_t pos;                  /**< Current position. */
    bool deny_write;            /**< Has file_deny_write() been called? */
    struct dir *dir;            /**< for readdir function*/
  };

/** Opening and closing files. */
struct file *file_open (struct inode *);
struct file *file_reopen (struct file *);
void file_close (struct file *);
struct inode *file_get_inode (struct file *);

uint32_t fdalloc(struct file *);
/** Reading and writing. */
off_t file_read (struct file *, void *, off_t);
off_t file_read_at (struct file *, void *, off_t size, off_t start);
off_t file_write (struct file *, const void *, off_t);
off_t file_write_at (struct file *, const void *, off_t size, off_t start);

/** Preventing writes. */
void file_deny_write (struct file *);
void file_allow_write (struct file *);

/** File position. */
void file_seek (struct file *, off_t);
off_t file_tell (struct file *);
off_t file_length (struct file *);

#endif /**< filesys/file.h */
