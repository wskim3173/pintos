#include <stdbool.h>
#include <stdint.h>
#include "lib/kernel/hash.h"
#include "filesys/file.h"

/* Backing type for a virtual page. */
enum vm_type {
  VM_ELF = 0,   /* Page backed by ELF executable segment (file-backed). */
  VM_ANON,      /* Anonymous page (zero-filled / stack). */
  VM_MMAP,      /* Memory-mapped file page. (later step) */
  VM_SWAP       /* Swapped-out page marker. (later step) */
};

/* Per-virtual-page metadata (Supplemental Page Table entry). */
struct vm_entry {
  void *upage;            /* User virtual page (page-aligned).            */
  bool writable;          /* Writable permission for this mapping.        */
  enum vm_type type;      /* Backing type.                                */

  /* File-backed metadata (VM_ELF / VM_MMAP). */
  struct file *file;      /* Backing file (borrowed ref).                 */
  off_t ofs;              /* File offset for the beginning of this page.  */
  size_t read_bytes;      /* Bytes to read from file.                     */
  size_t zero_bytes;      /* Bytes to zero after read.                    */

  /* Swap metadata (used in later steps). */
  size_t swap_slot;       /* Swap slot index if in swap, else (size_t)-1. */

  /* Residency state. */
  bool in_memory;         /* True iff a frame is currently mapped.        */

  struct hash_elem elem;  /* For membership in per-process SPT hash.      */
};

/* SPT (per-process) helpers. The SPT itself lives in thread_current()->vm. */
void vm_init(struct hash *vm);                 /* Initialize SPT hash.   */
void vm_destroy(struct hash *vm);              /* Destroy SPT & free vme.*/

struct vm_entry *find_vme(void *vaddr);        /* Lookup by user vaddr.  */
bool insert_vme(struct hash *vm, struct vm_entry *vme);
bool delete_vme(struct hash *vm, struct vm_entry *vme);