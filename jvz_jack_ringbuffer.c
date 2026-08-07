/*
 * jvz_jack_ringbuffer.c — lock-free SPSC ringbuffer (JACK-like create/free/write/read)
 *
 * One writer + one reader only. Pointer updates use acquire/release atomics so the
 * pattern is correct on weakly ordered CPUs (e.g. Apple Silicon).
 */

#include "jvz_jack_ringbuffer.h"

#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

static size_t load_acquire(const volatile size_t* p)
{
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

static void store_release(volatile size_t* p, size_t v)
{
    __atomic_store_n(p, v, __ATOMIC_RELEASE);
}

size_t jvz_jack_ringbuffer_read_space(const jvz_jack_ringbuffer_t* rb)
{
    if (rb == NULL)
        return 0u;

    const size_t w = load_acquire(&rb->write_ptr);
    const size_t r = rb->read_ptr; /* local reader view */

    if (w > r)
        return w - r;
    return (w - r + rb->size) & rb->size_mask;
}

/* Bytes available to write (leave one slot empty to distinguish full vs empty). */
size_t jvz_jack_ringbuffer_write_space(const jvz_jack_ringbuffer_t* rb)
{
    if (rb == NULL)
        return 0u;

    const size_t w = rb->write_ptr; /* local writer view */
    const size_t r = load_acquire(&rb->read_ptr);

    if (w > r)
        return ((r - w + rb->size) & rb->size_mask) - 1u;
    if (w < r)
        return (r - w) - 1u;
    return rb->size - 1u;
}

int jvz_jack_ringbuffer_mlock(jvz_jack_ringbuffer_t* rb)
{
    if (rb == NULL || rb->buf == NULL)
        return -1;
    if (mlock(rb->buf, rb->size) != 0)
        return -1;
    return 0;
}

jvz_jack_ringbuffer_t* jvz_jack_ringbuffer_create(size_t sz)
{
    if (sz == 0u)
        return NULL;

    jvz_jack_ringbuffer_t* rb = (jvz_jack_ringbuffer_t*)malloc(sizeof(*rb));
    if (rb == NULL)
        return NULL;

    /* Round capacity up to the next power of two (JACK semantics). */
    unsigned power = 1u;
    while (((size_t)1u << power) < sz)
    {
        power++;
        if (power >= sizeof(size_t) * 8u - 1u)
        {
            free(rb);
            return NULL;
        }
    }

    rb->size = (size_t)1u << power;
    rb->size_mask = rb->size - 1u;
    rb->write_ptr = 0u;
    rb->read_ptr = 0u;
    rb->bytes_written = 0u;
    rb->buf = (char*)malloc(rb->size);
    if (rb->buf == NULL)
    {
        free(rb);
        return NULL;
    }

    return rb;
}

void jvz_jack_ringbuffer_free(jvz_jack_ringbuffer_t* rb)
{
    if (rb == NULL)
        return;
    free(rb->buf);
    free(rb);
}

size_t jvz_jack_ringbuffer_write(jvz_jack_ringbuffer_t* rb, const char* src, size_t cnt)
{
    if (rb == NULL || src == NULL || cnt == 0u)
        return 0u;

    const size_t free_cnt = jvz_jack_ringbuffer_write_space(rb);
    if (free_cnt == 0u)
        return 0u;

    const size_t to_write = cnt > free_cnt ? free_cnt : cnt;
    size_t w = rb->write_ptr;
    const size_t cnt2 = w + to_write;
    size_t n1;
    size_t n2;

    if (cnt2 > rb->size)
    {
        n1 = rb->size - w;
        n2 = cnt2 & rb->size_mask;
    }
    else
    {
        n1 = to_write;
        n2 = 0u;
    }

    memcpy(&rb->buf[w], src, n1);
    w = (w + n1) & rb->size_mask;
    if (n2 != 0u)
    {
        memcpy(&rb->buf[w], src + n1, n2);
        w = (w + n2) & rb->size_mask;
    }

    store_release(&rb->write_ptr, w);
    {
        size_t bw = rb->bytes_written;
        if (bw < rb->size)
        {
            bw += to_write;
            if (bw > rb->size)
                bw = rb->size;
            store_release(&rb->bytes_written, bw);
        }
    }
    return to_write;
}

size_t jvz_jack_ringbuffer_read(jvz_jack_ringbuffer_t* rb, char* dest, size_t cnt)
{
    if (rb == NULL || dest == NULL || cnt == 0u)
        return 0u;

    const size_t avail = jvz_jack_ringbuffer_read_space(rb);
    if (avail == 0u)
        return 0u;

    const size_t to_read = cnt > avail ? avail : cnt;
    size_t r = rb->read_ptr;
    const size_t cnt2 = r + to_read;
    size_t n1;
    size_t n2;

    if (cnt2 > rb->size)
    {
        n1 = rb->size - r;
        n2 = cnt2 & rb->size_mask;
    }
    else
    {
        n1 = to_read;
        n2 = 0u;
    }

    memcpy(dest, &rb->buf[r], n1);
    r = (r + n1) & rb->size_mask;
    if (n2 != 0u)
    {
        memcpy(dest + n1, &rb->buf[r], n2);
        r = (r + n2) & rb->size_mask;
    }

    store_release(&rb->read_ptr, r);
    return to_read;
}

void jvz_jack_ringbuffer_read_advance(jvz_jack_ringbuffer_t* rb, size_t cnt)
{
    if (rb == NULL || cnt == 0u)
        return;

    const size_t avail = jvz_jack_ringbuffer_read_space(rb);
    if (cnt > avail)
        cnt = avail;

    store_release(&rb->read_ptr, (rb->read_ptr + cnt) & rb->size_mask);
}

size_t jvz_jack_ringbuffer_read_lastn(jvz_jack_ringbuffer_t* rb, char* dest, size_t n)
{
    if (rb == NULL || dest == NULL || n == 0u)
        return 0u;

    /* History window must fit in at most half the ring (FFT overlap / hop safety). */
    if (n > rb->size / 2u)
        return 0u;

    const size_t written = load_acquire(&rb->bytes_written);
    if (written < n)
        return 0u;

    /* Newest byte is just before write_ptr; copy the preceding `n` bytes. */
    const size_t w = load_acquire(&rb->write_ptr);
    size_t start = (w - n) & rb->size_mask;
    const size_t end = start + n;
    size_t n1;
    size_t n2;

    if (end > rb->size)
    {
        n1 = rb->size - start;
        n2 = end - rb->size;
    }
    else
    {
        n1 = n;
        n2 = 0u;
    }

    memcpy(dest, &rb->buf[start], n1);
    if (n2 != 0u)
        memcpy(dest + n1, rb->buf, n2);

    return n;
}
