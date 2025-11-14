#include "vm/page.h"
#include <string.h>
#include "threads/thread.h"
#include "threads/malloc.h"
#include "threads/vaddr.h"
#include "lib/kernel/hash.h"
#include "lib/debug.h"

/* -------- Internal helpers (hash callbacks) -------- */
static unsigned vm_hash_func (const struct hash_elem *e, void *aux UNUSED) {
  const struct vm_entry *vme = hash_entry (e, struct vm_entry, elem);
  return hash_bytes (&vme->upage, sizeof vme->upage);
}

static bool vm_less_func (const struct hash_elem *a,
                          const struct hash_elem *b,
                          void *aux UNUSED) {
  const struct vm_entry *va = hash_entry (a, struct vm_entry, elem);
  const struct vm_entry *vb = hash_entry (b, struct vm_entry, elem);
  return va->upage < vb->upage;
}

static void vm_destroy_func (struct hash_elem *e, void *aux UNUSED) {
  struct vm_entry *vme = hash_entry (e, struct vm_entry, elem);
  /* Step 1: just free the vm_entry. (Frames/swap will be handled later.) */
  free (vme);
}

/* -------- Public API -------- */
void vm_init (struct hash *vm) {
  ASSERT (vm != NULL);
  hash_init (vm, vm_hash_func, vm_less_func, NULL);
}

void vm_destroy (struct hash *vm) {
  ASSERT (vm != NULL);
  hash_destroy (vm, vm_destroy_func);
}

/* Find vm_entry for given user virtual address in current thread's SPT. */
struct vm_entry *find_vme (void *vaddr) {
  struct thread *t = thread_current ();
  void *upage = pg_round_down (vaddr);

  struct vm_entry key;
  memset (&key, 0, sizeof key);
  key.upage = upage;

  struct hash_elem *e = hash_find (&t->vm, &key.elem);
  return e ? hash_entry (e, struct vm_entry, elem) : NULL;
}

/* Insert a vm_entry into the given SPT. Returns false on duplicate. */
bool insert_vme (struct hash *vm, struct vm_entry *vme) {
  ASSERT (vm && vme);
  ASSERT (pg_ofs (vme->upage) == 0);  /* page-aligned key */
  struct hash_elem *prev = hash_insert (vm, &vme->elem);
  return prev == NULL;
}

/* Remove a vm_entry from the SPT and free it. */
bool delete_vme (struct hash *vm, struct vm_entry *vme) {
  ASSERT (vm && vme);
  struct hash_elem *e = hash_delete (vm, &vme->elem);
  if (e == NULL) return false;
  free (vme);
  return true;
}
