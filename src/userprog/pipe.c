#include "userprog/pipe.h"
#include "threads/malloc.h"
#include "threads/thread.h"
#include <string.h>

struct pipe *
pipe_create(void)
{
  struct pipe *p = malloc(sizeof *p);
  if (!p)
    return NULL;
  p->head = p->tail = p->count = 0;
  p->readers = 1; /* one read end */
  p->writers = 1; /* one write end */
  lock_init(&p->lk);
  cond_init(&p->can_read);
  cond_init(&p->can_write);
  p->closed_r = false;
  p->closed_w = false;
  return p;
}

void
pipe_ref_read_open(struct pipe *p)
{
  lock_acquire(&p->lk);
  p->readers++;
  lock_release(&p->lk);
}

void
pipe_ref_write_open(struct pipe *p)
{
  lock_acquire(&p->lk);
  p->writers++;
  lock_release(&p->lk);
}

void
pipe_destroy_if_unreferenced(struct pipe *p)
{
  if (!p)
    return;
  if (p->readers == 0 && p->writers == 0)
    free(p);
}

void
pipe_ref_read_close(struct pipe *p)
{
  lock_acquire(&p->lk);
  if (p->readers > 0)
    p->readers--;
  if (p->readers == 0)
  {
    p->closed_r = true;
    cond_broadcast(&p->can_write, &p->lk);
  }
  lock_release(&p->lk);
  pipe_destroy_if_unreferenced(p);
}

void
pipe_ref_write_close(struct pipe *p)
{
  lock_acquire(&p->lk);
  if (p->writers > 0)
    p->writers--;
  if (p->writers == 0)
  {
    p->closed_w = true;
    cond_broadcast(&p->can_read, &p->lk);
  }
  lock_release(&p->lk);
  pipe_destroy_if_unreferenced(p);
}

int
pipe_write(struct pipe *p, const void *ubuf, unsigned size)
{
  if (size == 0)
    return 0;
  lock_acquire(&p->lk);
  if (p->readers == 0)
  {
    lock_release(&p->lk);
    return -1;
  }

  unsigned written = 0;
  while (written < size)
  {
    while (p->count == PIPE_BUFSZ)
    {
      if (p->readers == 0)
      {
        lock_release(&p->lk);
        return -1;
      }
      cond_wait(&p->can_write, &p->lk);
    }

    size_t space = PIPE_BUFSZ - p->count;
    size_t chunk = space < (size_t)(size - written) ? space : (size_t)(size - written);

    size_t first = chunk;
    size_t endcap = PIPE_BUFSZ - (p->head % PIPE_BUFSZ);
    if (first > endcap)
      first = endcap;

    memcpy(&p->buf[p->head % PIPE_BUFSZ], (const uint8_t *)ubuf + written, first);
    p->head += first;
    p->count += first;
    written += first;

    size_t remain = chunk - first;
    if (remain)
    {
      memcpy(&p->buf[p->head % PIPE_BUFSZ], (const uint8_t *)ubuf + written, remain);
      p->head += remain;
      p->count += remain;
      written += remain;
    }

    cond_signal(&p->can_read, &p->lk);
  }

  lock_release(&p->lk);
  return (int)size;
}

int
pipe_read(struct pipe *p, void *ubuf, unsigned size)
{
  if (size == 0)
    return 0;
  lock_acquire(&p->lk);

  while (p->count == 0)
  {
    if (p->writers == 0)
    {
      lock_release(&p->lk);
      return 0;
    } /* EOF */
    cond_wait(&p->can_read, &p->lk);
  }

  size_t to_read = p->count < (size_t)size ? p->count : (size_t)size;

  size_t first = to_read;
  size_t endcap = PIPE_BUFSZ - (p->tail % PIPE_BUFSZ);
  if (first > endcap)
    first = endcap;

  memcpy((uint8_t *)ubuf, &p->buf[p->tail % PIPE_BUFSZ], first);
  p->tail += first;
  p->count -= first;

  size_t remain = to_read - first;
  if (remain)
  {
    memcpy((uint8_t *)ubuf + first, &p->buf[p->tail % PIPE_BUFSZ], remain);
    p->tail += remain;
    p->count -= remain;
  }

  cond_signal(&p->can_write, &p->lk);

  lock_release(&p->lk);
  return (int)to_read;
}