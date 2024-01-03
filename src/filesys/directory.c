#include "filesys/directory.h"
#include <stdio.h>
#include <string.h>
#include <list.h>
#include "filesys/filesys.h"
#include "filesys/inode.h"
#include "threads/malloc.h"


/** Creates a directory with space for ENTRY_CNT entries in the
   given SECTOR.  Returns true if successful, false on failure. */
bool
dir_create (block_sector_t sector, size_t entry_cnt)
{
  bool success = inode_create (sector, entry_cnt * sizeof (struct dir_entry), true, true);
  if (success) {
    struct dir *dir = dir_open_root();
    dir_add(dir, ".", ROOT_DIR_SECTOR);
    dir_add(dir, "..", ROOT_DIR_SECTOR);
  }
  return success;
}

/** Opens and returns the directory for the given INODE, of which
   it takes ownership.  Returns a null pointer on failure. */
struct dir *
dir_open (struct inode *inode) 
{
  struct dir *dir = calloc (1, sizeof *dir);
  if (inode != NULL && dir != NULL)
    {
      dir->inode = inode;
      dir->pos = 0;
      return dir;
    }
  else
    {
      inode_close (inode);
      free (dir);
      return NULL; 
    }
}

/** Opens the root directory and returns a directory for it.
   Return true if successful, false on failure. */
struct dir *
dir_open_root (void)
{
  return dir_open (inode_open (ROOT_DIR_SECTOR));
}

/** Opens and returns a new directory for the same inode as DIR.
   Returns a null pointer on failure. */
struct dir *
dir_reopen (struct dir *dir) 
{
  return dir_open (inode_reopen (dir->inode));
}

/** Destroys DIR and frees associated resources. */
void
dir_close (struct dir *dir) 
{
  if (dir != NULL)
    {
      inode_close (dir->inode);
      free (dir);
    }
}

/** Returns the inode encapsulated by DIR. */
struct inode *
dir_get_inode (struct dir *dir) 
{
  return dir->inode;
}

/** Searches DIR for a file with the given NAME.
   If successful, returns true, sets *EP to the directory entry
   if EP is non-null, and sets *OFSP to the byte offset of the
   directory entry if OFSP is non-null.
   otherwise, returns false and ignores EP and OFSP. */
static bool
lookup (const struct dir *dir, const char *name,
        struct dir_entry *ep, off_t *ofsp) 
{
  struct dir_entry e;
  size_t ofs;
  
  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  for (ofs = 0; inode_read_at (dir->inode, &e, sizeof e, ofs) == sizeof e;
       ofs += sizeof e) 
    if (e.in_use && !strcmp (name, e.name)) 
      {
        if (ep != NULL)
          *ep = e;
        if (ofsp != NULL)
          *ofsp = ofs;
        return true;
      }
  return false;
}
/** Searches DIR for a file with the given NAME
   and returns true if one exists, false otherwise.
   On success, sets *INODE to an inode for the file, otherwise to
   a null pointer.  The caller must close *INODE. */
bool
dir_lookup (const struct dir *dir, const char *name,
            struct inode **inode) 
{
  struct dir_entry e;

  ASSERT (dir != NULL);
  ASSERT (name != NULL);
  // 不允许在已经删除的inode中搜索
  if (is_inode_removed(dir->inode)) return false;
  if (!is_inode_dir(dir->inode)) return false;
  if (lookup (dir, name, &e, NULL))
    *inode = inode_open (e.inode_sector);
  else
    *inode = NULL;

  return *inode != NULL;
}

/** Adds a file named NAME to DIR, which must not already contain a
   file by that name.  The file's inode is in sector
   INODE_SECTOR.
   Returns true if successful, false on failure.
   Fails if NAME is invalid (i.e. too long) or a disk or memory
   error occurs. */
// 一般来讲向目录中添加引用inode_sector的目录项应该增加这个inode的硬链接数，目前本系统中没有实现
// 硬链接，所以在创建inode时（inode_create）直接将硬链接数置为1，使用dir_add时不改变硬链接数（比如
// 增加..和.目录项都不改变对上一级inode和当前inode的硬链接数）
bool
dir_add (struct dir *dir, const char *name, block_sector_t inode_sector)
{
  struct dir_entry e;
  off_t ofs;
  bool success = false;

  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  /* Check NAME for validity. */
  if (*name == '\0' || strlen (name) > NAME_MAX)
    return false;

  /* Check that NAME is not in use. */
  if (lookup (dir, name, NULL, NULL))
    goto done;

  /* Set OFS to offset of free slot.
     If there are no free slots, then it will be set to the
     current end-of-file.
     
     inode_read_at() will only return a short read at end of file.
     Otherwise, we'd need to verify that we didn't get a short
     read due to something intermittent such as low memory. */
  // 读取目录的inode的数据块，从0开始，一条一条地读取目录项到e中，找到空闲的目录项
  for (ofs = 0; inode_read_at (dir->inode, &e, sizeof e, ofs) == sizeof e;
       ofs += sizeof e) 
    if (!e.in_use)
      break;

  /* Write slot. */
  e.in_use = true;
  strlcpy (e.name, name, sizeof e.name);
  e.inode_sector = inode_sector;
  success = inode_write_at (dir->inode, &e, sizeof e, ofs) == sizeof e;
 done:
  return success;
}
// 除了.和..之外，目录是否为空
static bool
is_dir_empty(struct inode *inode) {
  ASSERT(is_inode_dir(inode));
  struct dir_entry e;
  for (int off = 2 * sizeof(struct dir_entry); inode_read_at(inode, &e, sizeof e, off) == sizeof e; off += sizeof e) {
    if (e.in_use) return false;
  }
  return true;
}
/** Removes any entry for NAME in DIR.
   Returns true if successful, false on failure,
   which occurs only if there is no file with the given NAME. */
bool
dir_remove (struct dir *dir, const char *name) 
{
  struct dir_entry e;
  struct inode *inode = NULL;
  bool success = false;
  off_t ofs;

  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  /* Find directory entry. */
  if (!lookup (dir, name, &e, &ofs))
    goto done;
  /* Open inode. */
  inode = inode_open (e.inode_sector);
  if (inode == NULL)
    goto done;
  // 如果要remove的对象是目录，并且目录不为空，那么失败
  if (is_inode_dir(inode) && !is_dir_empty(inode)) {
    goto done;  
  }
  /* Erase directory entry. */
  e.in_use = false;
  if (inode_write_at (dir->inode, &e, sizeof e, ofs) != sizeof e) 
    goto done;

  /* Remove inode. */
  inode_unlink(inode);
  success = true;

 done:
  inode_close (inode);
  return success;
}

/** Reads the next directory entry in DIR and stores the name in
   NAME.  Returns true if successful, false if the directory
   contains no more entries. */
bool
dir_readdir (struct dir *dir, char name[NAME_MAX + 1])
{
  struct dir_entry e;

  while (inode_read_at (dir->inode, &e, sizeof e, dir->pos) == sizeof e) 
    {
      dir->pos += sizeof e;
      if (e.in_use)
        {
          strlcpy (name, e.name, NAME_MAX + 1);
          return true;
        } 
    }
  return false;
}

// paths

// 解析给定的路径名path
// 它提取出path中的下一个元素，并拷贝到name中，然后返回在下个元素之后的后续路径
// Examples:
//   skipelem("a/bb/c", name) = "bb/c", setting name = "a"
//   skipelem("///a//bb", name) = "bb", setting name = "a"
//   skipelem("a", name) = "", setting name = "a"
//   skipelem("", name) = skipelem("////", name) = 0
//
static char*
skipelem(char *path, char *name, bool *is_valid) {
  while (*path == '/') path++;
  // 如果path的元素是0，那么说明path中没有任何元素，直接返回NULL
  if (*path == '\0') return NULL;
  char *s = path;
  // 搜索到/或者0停止
  while (*path != '/' && *path != '\0') path++;
  int len = path - s;
  // 文件名大于合法的长度，则直接返回NULL
  if (len > NAME_MAX) {
    *is_valid = false;
    return NULL;
  } else {
    memcpy(name, s, len);
    name[len] = '\0';
  }
  // 最后将指针推进到斜杠之外的下一个元素
  while (*path == '/') path++;
  return path;
}

static struct dir*
namex(char *path, int nameiparent, char *name)
{
  struct dir *dir;
  if (*path == 0) return NULL;
  if (*path == '/') dir = dir_open_root();
  else dir = dir_reopen(thread_current()->cwd);
  struct inode *next = NULL;
  bool is_valid = true;
  // 如果没有下一个元素那么就结束了
  while((path = skipelem(path, name, &is_valid)) != 0){
    // 如果我们要获取父节点的inode，那么当剩下的path是空的时，就可以返回。
    // 此时已经获取了path中的最后一个元素name，dir中的inode也是name的上一级inode
    if(nameiparent && *path == '\0'){
      return dir;
    }
    if(!dir_lookup(dir, name, &next)){
      dir_close(dir);
      return NULL;
    }
    dir_close(dir);
    dir = dir_open(next);
  }
  if(nameiparent || !is_valid){
    dir_close(dir);
    return NULL;
  }
  return dir;
}
// namei返回路径名中最后一个元素的inode
// 如果没有找到就返回NULL
struct dir*
name_inode(char *path)
{
  char name[NAME_MAX];
  return namex(path, 0, name);
}
// nameiparent返回最后一个元素的父目录的inode，
// 并且将最后一个元素的名称复制到调用者指定的位置*name中
// 比name_inode提前停止一个等级
struct dir*
name_inode_parent(char *path, char *name)
{
  return namex(path, 1, name);
}