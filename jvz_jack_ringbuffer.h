/*
 * jvz_jack_ringbuffer.h — lock-free single-reader / single-writer byte ringbuffer
 *
 * API mirrors jack_ringbuffer_{create,free,write,read}. Safe for one writer thread
 * and one reader thread concurrently; identities must not be swapped.
 */

#ifndef JVZ_JACK_RINGBUFFER_H
#define JVZ_JACK_RINGBUFFER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct JvzJackRingbuffer
{
    char* buf;
    size_t write_ptr;
    size_t read_ptr;
    size_t size;      /* power of two */
    size_t size_mask; /* size - 1 */
} jvz_jack_ringbuffer_t;

/* Allocate a ringbuffer of at least `sz` bytes (rounded up to a power of two). */
jvz_jack_ringbuffer_t* jvz_jack_ringbuffer_create(size_t sz);

/* Free a ringbuffer from jvz_jack_ringbuffer_create(). */
void jvz_jack_ringbuffer_free(jvz_jack_ringbuffer_t* rb);

/* Copy at most `cnt` bytes from `src` into `rb`. Returns bytes written (0 … cnt). */
size_t jvz_jack_ringbuffer_write(jvz_jack_ringbuffer_t* rb, const char* src, size_t cnt);

/* Copy at most `cnt` bytes from `rb` into `dest`. Returns bytes read (0 … cnt). */
size_t jvz_jack_ringbuffer_read(jvz_jack_ringbuffer_t* rb, char* dest, size_t cnt);

#ifdef __cplusplus
}
#endif

#endif /* JVZ_JACK_RINGBUFFER_H */
