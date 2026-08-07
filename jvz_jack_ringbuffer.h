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
    size_t size;          /* power of two */
    size_t size_mask;     /* size - 1 */
    size_t bytes_written; /* saturates at size; used by read_lastn */
} jvz_jack_ringbuffer_t;

/* Allocate a ringbuffer of at least `sz` bytes (rounded up to a power of two). */
jvz_jack_ringbuffer_t* jvz_jack_ringbuffer_create(size_t sz);

/* Free a ringbuffer from jvz_jack_ringbuffer_create(). */
void jvz_jack_ringbuffer_free(jvz_jack_ringbuffer_t* rb);

/* Copy at most `cnt` bytes from `src` into `rb`. Returns bytes written (0 … cnt). */
size_t jvz_jack_ringbuffer_write(jvz_jack_ringbuffer_t* rb, const char* src, size_t cnt);

/* Copy at most `cnt` bytes from `rb` into `dest`. Returns bytes read (0 … cnt). */
size_t jvz_jack_ringbuffer_read(jvz_jack_ringbuffer_t* rb, char* dest, size_t cnt);

/* Advance the read pointer by `cnt` bytes (same API as jack_ringbuffer_read_advance). */
void jvz_jack_ringbuffer_read_advance(jvz_jack_ringbuffer_t* rb, size_t cnt);

/* Bytes available to read (same API as jack_ringbuffer_read_space). */
size_t jvz_jack_ringbuffer_read_space(const jvz_jack_ringbuffer_t* rb);

/* Bytes available to write (same API as jack_ringbuffer_write_space). */
size_t jvz_jack_ringbuffer_write_space(const jvz_jack_ringbuffer_t* rb);

/* Lock the ringbuffer data into RAM (same API as jack_ringbuffer_mlock). */
int jvz_jack_ringbuffer_mlock(jvz_jack_ringbuffer_t* rb);

/*
 * Copy the last `n` bytes written into `dest`, looking back from the write pointer.
 * Independent of the consumer read pointer (does not advance it), so `n` may be larger
 * or smaller than any single prior write — useful for FFT history / overlap.
 *
 * Requires `n <= size/2` and that at least `n` bytes have been written overall.
 * Returns `n` on success, or 0 if the request is invalid / not yet enough history.
 */
size_t jvz_jack_ringbuffer_read_lastn(jvz_jack_ringbuffer_t* rb, char* dest, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* JVZ_JACK_RINGBUFFER_H */
