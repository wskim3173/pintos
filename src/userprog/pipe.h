#ifndef USERPROG_PIPE_H
#define USERPROG_PIPE_H
#include "threads/synch.h"
#include <stddef.h>
#include <stdbool.h>

#define PIPE_BUFSZ 4096 /* fixed ring buffer; writers must block until size bytes written */

struct pipe
{
    /* ring buffer */
    uint8_t buf[PIPE_BUFSZ];
    size_t head;  /* write index */
    size_t tail;  /* read index */
    size_t count; /* bytes in buf */

    /* endpoints */
    int readers; /* open read ends (lab: 1) */
    int writers; /* open write ends (lab: 1) */

    /* sync: classic producer/consumer */
    struct lock lk;             /* protects state below */
    struct condition can_read;  /* signaled when count>0 or writers drop to 0 */
    struct condition can_write; /* signaled when count<PIPE_BUFSZ or readers drop to 0 */

    bool closed_r; /* all read ends closed */
    bool closed_w; /* all write ends closed */
};

struct pipe *pipe_create(void);
void pipe_ref_read_open(struct pipe *p);
void pipe_ref_write_open(struct pipe *p);
void pipe_ref_read_close(struct pipe *p);
void pipe_ref_write_close(struct pipe *p);
void pipe_destroy_if_unreferenced(struct pipe *p);

/* data paths */
int pipe_write(struct pipe *p, const void *ubuf, unsigned size);
int pipe_read(struct pipe *p, void *ubuf, unsigned size);

#endif