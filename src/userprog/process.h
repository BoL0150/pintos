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

#endif /**< userprog/process.h */
