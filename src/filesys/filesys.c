#include "filesys/buf.h"
#include "userprog/syscall.h"
#include "lib/string.h"
#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"

/** Partition that contains the file system. */
struct block *fs_device;
extern struct lock filesys_lock;
extern struct list ready_list;
extern bool continue_flush;
static void do_format (void);
/** Initializes the file system module.
   If FORMAT is true, reformats the file system. */
void
filesys_init (bool format) 
{
  fs_device = block_get_role (BLOCK_FILESYS);
  if (fs_device == NULL)
    PANIC ("No file system device found, can't initialize file system.");
  filesysBufCacheInit(); 
  inode_init ();
  free_map_init ();

  if (format) 
    do_format ();

  free_map_open ();
}

/** Shuts down the file system module, writing any unwritten data
   to disk. */
void
filesys_done (void) 
{
  free_map_close ();
  continue_flush = false;
  flush();
}

/** Creates a file named NAME with the given INITIAL_SIZE.
   Returns true if successful, false otherwise.
   Fails if a file named NAME already exists,
   or if internal memory allocation fails. */
bool
filesys_create (const char *path, off_t initial_size, bool is_dir) 
{
  if (list_size(&ready_list) != 0) {
    ASSERT(lock_held_by_current_thread(&filesys_lock));
  }
  block_sector_t inode_sector = 0;
  char name[NAME_MAX];
  struct dir *dir = name_inode_parent(path, name);
  struct inode *inode;
  // 要求path中最后一个元素之前的所有元素都存在（dir != NULL)，并且最后一个元素不存在
  bool success = (dir != NULL
                  && !dir_lookup(dir, name, &inode)
                  && free_map_allocate (1, &inode_sector)
                  && inode_create(inode_sector, initial_size, true, is_dir)
                  && dir_add(dir, name, inode_sector));
  if (!success && inode_sector != 0) 
    free_map_release(inode_sector, 1);
  dir_close(dir);
  return success;
}
/** Opens the file with the given NAME.
   Returns the new file if successful or a null pointer
   otherwise.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
struct file *
filesys_open (const char *name)
{
  struct dir* dir = name_inode(name);
  if (dir == NULL) return NULL;
  struct file *f = file_open(inode_reopen(dir_get_inode(dir)));
  if (f != NULL) strlcpy(f->name, name, strlen(name) + 1);
  dir_close(dir);
  return f;

  // struct dir *dir = dir_open_root ();
  // struct inode *inode = NULL;

  // if (dir != NULL)
  //   dir_lookup (dir, name, &inode);
  // dir_close (dir);
  // struct file *f = file_open (inode);
  // if (f != NULL) strlcpy(f->name, name, strlen(name) + 1);
  // return f;
  // return file_open (inode);
}

/** Deletes the file named NAME.
   Returns true if successful, false on failure.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
bool
filesys_remove (const char *name) 
{
  if (list_size(&ready_list) != 0) {
    ASSERT(lock_held_by_current_thread(&filesys_lock));
  }
  char remove_name[NAME_MAX];
  struct dir *parent_dir = name_inode_parent(name, remove_name);
  bool success = ((parent_dir != NULL) && dir_remove(parent_dir, remove_name));
  dir_close(parent_dir);
  return success;
}

/** Formats the file system. */
static void
do_format (void)
{
  printf ("Formatting file system...");
  free_map_create ();
  if (!dir_create (ROOT_DIR_SECTOR, 16))
    PANIC ("root directory creation failed");
  free_map_close ();
  printf ("done.\n");
}
