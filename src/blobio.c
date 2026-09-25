/*
 * Copyright 2010-2026 The pygit2 contributors
 *
 * This file is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2,
 * as published by the Free Software Foundation.
 *
 * In addition to the permissions in the GNU General Public License,
 * the authors give you unlimited permission to link the compiled
 * version of this file into combinations with other programs,
 * and to distribute those combinations without any restriction
 * coming from the use of this file.  (The General Public License
 * restrictions do apply in other respects; for example, they cover
 * modification of the file, and distribution when not linked into
 * a combined executable.)
 *
 * This file is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

/*
 * _BlobRing: the buffer between BlobIO's writer thread (libgit2 streaming a
 * blob) and the reader (BlobIO.read).
 *
 * Single producer, single consumer. The writer's git_writestream copies
 * libgit2's output straight into fixed-size slots, with the interpreter
 * released and without creating Python objects; the reader copies out of the
 * slots into the caller's buffer.
 *
 * `head` is only written by the writer and `tail` only by the reader; each
 * lives on its own cache line and each side caches the other's index. A side
 * that must wait spins briefly, then sleeps on a condition variable. The other
 * side only takes the mutex to signal when `waiters` is non-zero. Both the
 * `waiters` increment and the index publish are sequentially consistent, so
 * either the sleeper sees the new index or the waker sees the sleeper: no
 * wakeup can be lost.
 *
 * Slots are allocated lazily, when the writer first needs them. The first slot
 * is sized from `size_hint` (the blob size) so small blobs don't allocate full
 * slots; filtered output may be larger than the hint, later slots are full
 * size.
 */

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <git2.h>
#include <git2/sys/errors.h>
#include <string.h>
#include "blobio.h"

/* Portability: atomics, mutex, condition variable. */

#ifdef _WIN32

#include <windows.h>

typedef volatile LONG64 ring_size_t;  /* holds a size_t */
typedef volatile LONG ring_int_t;
typedef SRWLOCK ring_mutex_t;
typedef CONDITION_VARIABLE ring_cond_t;

static inline size_t ring_load(ring_size_t *p) { return (size_t)InterlockedCompareExchange64(p, 0, 0); }
static inline void ring_store(ring_size_t *p, size_t v) { InterlockedExchange64(p, (LONG64)v); }
static inline int ring_load_int(ring_int_t *p) { return (int)InterlockedCompareExchange(p, 0, 0); }
static inline void ring_store_int(ring_int_t *p, int v) { InterlockedExchange(p, v); }
static inline void ring_add_int(ring_int_t *p, int v) { InterlockedExchangeAdd(p, v); }

static inline void ring_mutex_init(ring_mutex_t *m) { InitializeSRWLock(m); }
static inline void ring_mutex_destroy(ring_mutex_t *m) { (void)m; }
static inline void ring_lock(ring_mutex_t *m) { AcquireSRWLockExclusive(m); }
static inline void ring_unlock(ring_mutex_t *m) { ReleaseSRWLockExclusive(m); }
static inline void ring_cond_init(ring_cond_t *c) { InitializeConditionVariable(c); }
static inline void ring_cond_destroy(ring_cond_t *c) { (void)c; }
static inline void ring_broadcast(ring_cond_t *c) { WakeAllConditionVariable(c); }
static inline void ring_cond_wait(ring_cond_t *c, ring_mutex_t *m)
{
    SleepConditionVariableSRW(c, m, INFINITE, 0);
}
/* Returns 0 on timeout. */
static inline int ring_cond_timedwait(ring_cond_t *c, ring_mutex_t *m, unsigned ms)
{
    return SleepConditionVariableSRW(c, m, ms, 0) ? 1 : 0;
}
#define ring_cpu_relax() YieldProcessor()
#define ring_aligned_alloc(align, size) _aligned_malloc((size), (align))
#define ring_aligned_free(p) _aligned_free(p)

#else

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <time.h>

typedef _Atomic size_t ring_size_t;
typedef _Atomic int ring_int_t;
typedef pthread_mutex_t ring_mutex_t;
typedef pthread_cond_t ring_cond_t;

static inline size_t ring_load(ring_size_t *p) { return atomic_load(p); }
static inline void ring_store(ring_size_t *p, size_t v) { atomic_store(p, v); }
static inline int ring_load_int(ring_int_t *p) { return atomic_load(p); }
static inline void ring_store_int(ring_int_t *p, int v) { atomic_store(p, v); }
static inline void ring_add_int(ring_int_t *p, int v) { atomic_fetch_add(p, v); }

static inline void ring_mutex_init(ring_mutex_t *m) { pthread_mutex_init(m, NULL); }
static inline void ring_mutex_destroy(ring_mutex_t *m) { pthread_mutex_destroy(m); }
static inline void ring_lock(ring_mutex_t *m) { pthread_mutex_lock(m); }
static inline void ring_unlock(ring_mutex_t *m) { pthread_mutex_unlock(m); }
static inline void ring_cond_init(ring_cond_t *c) { pthread_cond_init(c, NULL); }
static inline void ring_cond_destroy(ring_cond_t *c) { pthread_cond_destroy(c); }
static inline void ring_broadcast(ring_cond_t *c) { pthread_cond_broadcast(c); }
static inline void ring_cond_wait(ring_cond_t *c, ring_mutex_t *m)
{
    pthread_cond_wait(c, m);
}
/* Returns 0 on timeout. */
static inline int ring_cond_timedwait(ring_cond_t *c, ring_mutex_t *m, unsigned ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000L;
    }
    return pthread_cond_timedwait(c, m, &ts) == 0;
}
#if defined(__aarch64__)
#define ring_cpu_relax() __asm__ __volatile__("yield")
#elif defined(__x86_64__) || defined(__i386__)
#define ring_cpu_relax() __builtin_ia32_pause()
#else
#define ring_cpu_relax() ((void)0)
#endif
#define ring_aligned_alloc(align, size) aligned_alloc((align), (size))
#define ring_aligned_free(p) free(p)

#endif

/* 128 covers Apple M-series; x86 lines are 64 bytes. */
#define RING_CACHE_LINE 128
#define RING_SPINS 2000
#define RING_MIN_SLOT 4096
/* How often a waiting reader wakes up to check for signals (Ctrl-C). */
#define RING_SIGNAL_CHECK_MS 100

typedef struct {
    ring_size_t head;       /* written by the writer */
    size_t tail_cache;      /* writer's last seen tail */
    char pad0[RING_CACHE_LINE - sizeof(ring_size_t) - sizeof(size_t)];
    ring_size_t tail;       /* written by the reader */
    size_t head_cache;      /* reader's last seen head */
    char pad1[RING_CACHE_LINE - sizeof(ring_size_t) - sizeof(size_t)];
    ring_int_t eof;
    ring_int_t cancelled;
    ring_int_t waiters;
    ring_mutex_t mu;
    ring_cond_t cv;
} ring_ctrl;

struct BlobRing {
    PyObject_HEAD
    ring_ctrl *c;           /* RING_CACHE_LINE aligned */
    char **slot;            /* allocated lazily by the writer */
    size_t *cap;            /* capacity of each allocated slot */
    size_t *len;            /* published length of each slot */
    size_t nslots, mask, slot_size, size_hint;
    int wopen;              /* writer only: a slot is being filled */
    size_t wfill;           /* writer only */
    size_t roff;            /* reader only: offset in the current slot */
};

typedef struct {
    git_writestream base;
    BlobRing *ring;
} ring_stream;


/* Waiting. These never touch Python. */

typedef int (*ring_pred)(BlobRing *);

static int ring_can_put(BlobRing *r)
{
    return ring_load(&r->c->head) - ring_load(&r->c->tail) < r->nslots
           || ring_load_int(&r->c->cancelled);
}

static int ring_can_get(BlobRing *r)
{
    return ring_load(&r->c->head) != ring_load(&r->c->tail)
           || ring_load_int(&r->c->eof);
}

static void ring_wake(ring_ctrl *c)
{
    if (ring_load_int(&c->waiters) > 0) {
        ring_lock(&c->mu);
        ring_broadcast(&c->cv);
        ring_unlock(&c->mu);
    }
}

/* Wait until ok(r), at most `ms` milliseconds (0 = no limit).
 * Returns 1 if ok(r), 0 on timeout. */
static int ring_wait(BlobRing *r, ring_pred ok, unsigned ms)
{
    int i, ready = 1;

    for (i = 0; i < RING_SPINS && !ok(r); i++)
        ring_cpu_relax();
    if (ok(r))
        return 1;

    ring_lock(&r->c->mu);
    ring_add_int(&r->c->waiters, 1);
    while (!ok(r)) {
        if (ms == 0) {
            ring_cond_wait(&r->c->cv, &r->c->mu);
        } else if (!ring_cond_timedwait(&r->c->cv, &r->c->mu, ms)) {
            ready = ok(r);
            break;
        }
    }
    ring_add_int(&r->c->waiters, -1);
    ring_unlock(&r->c->mu);
    return ready;
}


/* Writer: a git_writestream fed by libgit2, interpreter released. */

static void ring_publish(BlobRing *r)
{
    size_t head = ring_load(&r->c->head);

    r->len[head & r->mask] = r->wfill;
    ring_store(&r->c->head, head + 1);
    ring_wake(r->c);
    r->wopen = 0;
    r->wfill = 0;
}

/* Wait for a free slot and make sure it's allocated. */
static int ring_claim(BlobRing *r)
{
    ring_ctrl *c = r->c;
    size_t head = ring_load(&c->head);
    size_t idx, want;
    char *p;

    while (head - c->tail_cache >= r->nslots) {
        c->tail_cache = ring_load(&c->tail);
        if (head - c->tail_cache < r->nslots)
            break;
        if (ring_load_int(&c->cancelled))
            return -1;
        ring_wait(r, ring_can_put, 0);
    }
    if (ring_load_int(&c->cancelled))
        return -1;

    idx = head & r->mask;
    want = r->slot_size;
    if (head == 0 && r->size_hint > 0 && r->size_hint < want)
        want = r->size_hint < RING_MIN_SLOT ? RING_MIN_SLOT : r->size_hint;
    if (r->slot[idx] == NULL || r->cap[idx] < want) {
        p = PyMem_RawRealloc(r->slot[idx], want);
        if (p == NULL)
            return -1;
        r->slot[idx] = p;
        r->cap[idx] = want;
    }
    r->wopen = 1;
    r->wfill = 0;
    return 0;
}

static int ring_stream_write(git_writestream *s, const char *buffer, size_t len)
{
    BlobRing *r = ((ring_stream *)s)->ring;
    size_t idx, n;

    while (len > 0) {
        if (!r->wopen && ring_claim(r) < 0) {
            git_error_set(GIT_ERROR_OS, "blob stream closed by the reader");
            return -1;
        }
        idx = ring_load(&r->c->head) & r->mask;
        n = r->cap[idx] - r->wfill;
        if (n > len)
            n = len;
        memcpy(r->slot[idx] + r->wfill, buffer, n);
        r->wfill += n;
        buffer += n;
        len -= n;
        if (r->wfill == r->cap[idx])
            ring_publish(r);
    }
    return 0;
}

static int ring_stream_close(git_writestream *s)
{
    BlobRing *r = ((ring_stream *)s)->ring;

    if (r->wopen && r->wfill > 0)
        ring_publish(r);
    return 0;
}

static void ring_stream_free(git_writestream *s)
{
    (void)s;
}

int BlobRing_stream(BlobRing *ring, git_filter_list *fl, git_blob *blob)
{
    ring_stream ws;

    memset(&ws, 0, sizeof(ws));
    ws.base.write = ring_stream_write;
    ws.base.close = ring_stream_close;
    ws.base.free = ring_stream_free;
    ws.ring = ring;
    return git_filter_list_stream_blob(fl, blob, &ws.base);
}

/* Size the first slot from the blob, unless the caller gave a hint. Writer
 * side, before streaming starts. */
void BlobRing_set_size_hint(BlobRing *ring, size_t size)
{
    if (ring->size_hint == 0)
        ring->size_hint = size;
}

void BlobRing_finish(BlobRing *ring)
{
    ring_store_int(&ring->c->eof, 1);
    ring_wake(ring->c);
}

int BlobRing_cancelled(BlobRing *ring)
{
    return ring_load_int(&ring->c->cancelled);
}


/* Python type. */

static int
BlobRing_init(BlobRing *self, PyObject *args, PyObject *kwds)
{
    char *keywords[] = {"slots", "slot_size", "size_hint", NULL};
    Py_ssize_t slots, slot_size, size_hint = 0;
    size_t n = 1, ctrl_size;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "nn|n", keywords,
                                     &slots, &slot_size, &size_hint))
        return -1;
    if (slots < 1 || slot_size < 1 || size_hint < 0) {
        PyErr_SetString(PyExc_ValueError,
                        "slots and slot_size must be positive, size_hint not negative");
        return -1;
    }
    if (self->c != NULL) {
        PyErr_SetString(PyExc_RuntimeError, "_BlobRing already initialized");
        return -1;
    }

    while (n < (size_t)slots)
        n <<= 1;
    ctrl_size = (sizeof(ring_ctrl) + RING_CACHE_LINE - 1) / RING_CACHE_LINE * RING_CACHE_LINE;
    self->c = ring_aligned_alloc(RING_CACHE_LINE, ctrl_size);
    self->slot = PyMem_RawCalloc(n, sizeof(char *));
    self->cap = PyMem_RawCalloc(n, sizeof(size_t));
    self->len = PyMem_RawCalloc(n, sizeof(size_t));
    if (self->c == NULL || self->slot == NULL || self->cap == NULL || self->len == NULL) {
        PyErr_NoMemory();
        return -1;
    }
    memset(self->c, 0, ctrl_size);
    ring_mutex_init(&self->c->mu);
    ring_cond_init(&self->c->cv);
    self->nslots = n;
    self->mask = n - 1;
    self->slot_size = (size_t)slot_size;
    self->size_hint = (size_t)size_hint;
    return 0;
}

static void
BlobRing_dealloc(BlobRing *self)
{
    size_t i;

    if (self->slot != NULL) {
        for (i = 0; i < self->nslots; i++)
            PyMem_RawFree(self->slot[i]);
    }
    PyMem_RawFree(self->slot);
    PyMem_RawFree(self->cap);
    PyMem_RawFree(self->len);
    if (self->c != NULL) {
        ring_mutex_destroy(&self->c->mu);
        ring_cond_destroy(&self->c->cv);
        ring_aligned_free(self->c);
    }
    Py_TYPE(self)->tp_free((PyObject *)self);
}

#define CHECK_RING_INIT(self) \
    if ((self)->c == NULL) { \
        PyErr_SetString(PyExc_ValueError, "_BlobRing not initialized"); \
        return NULL; \
    }

PyDoc_STRVAR(BlobRing_readinto__doc__,
  "readinto(buffer) -> int\n"
  "\n"
  "Copy the next bytes written by the writer into `buffer`, blocking until\n"
  "some are available. Returns the number of bytes copied, 0 at the end of\n"
  "the stream.");

static PyObject *
BlobRing_readinto(BlobRing *self, PyObject *py_buffer)
{
    Py_buffer view;
    ring_ctrl *c;
    size_t t, idx, avail, n = 0;
    int ready;

    CHECK_RING_INIT(self);
    c = self->c;
    if (PyObject_GetBuffer(py_buffer, &view, PyBUF_WRITABLE) < 0)
        return NULL;

    while (view.len > 0) {
        t = ring_load(&c->tail);
        if (c->head_cache == t) {
            c->head_cache = ring_load(&c->head);
            if (c->head_cache == t) {
                if (ring_load_int(&c->eof)) {
                    /* eof is set after the last publish: re-read head */
                    c->head_cache = ring_load(&c->head);
                    if (c->head_cache == t)
                        break;
                    continue;
                }
                Py_BEGIN_ALLOW_THREADS
                ready = ring_wait(self, ring_can_get, RING_SIGNAL_CHECK_MS);
                Py_END_ALLOW_THREADS
                if (!ready && PyErr_CheckSignals() < 0) {
                    PyBuffer_Release(&view);
                    return NULL;
                }
                continue;
            }
        }
        idx = t & self->mask;
        avail = self->len[idx] - self->roff;
        n = avail < (size_t)view.len ? avail : (size_t)view.len;
        memcpy(view.buf, self->slot[idx] + self->roff, n);
        self->roff += n;
        if (self->roff == self->len[idx]) {
            self->roff = 0;
            ring_store(&c->tail, t + 1);
            ring_wake(c);
        }
        if (n > 0)
            break;
    }

    PyBuffer_Release(&view);
    return PyLong_FromSize_t(n);
}

PyDoc_STRVAR(BlobRing_close__doc__,
  "close()\n"
  "\n"
  "Reader side: stop the writer. Pending and later writes fail, which makes\n"
  "libgit2 stop streaming.");

static PyObject *
BlobRing_close(BlobRing *self, PyObject *Py_UNUSED(ignored))
{
    CHECK_RING_INIT(self);
    ring_store_int(&self->c->cancelled, 1);
    ring_wake(self->c);
    Py_RETURN_NONE;
}

PyDoc_STRVAR(BlobRing_close_write__doc__,
  "close_write()\n"
  "\n"
  "Writer side: signal the end of the stream. Safe to call more than once.");

static PyObject *
BlobRing_close_write(BlobRing *self, PyObject *Py_UNUSED(ignored))
{
    CHECK_RING_INIT(self);
    BlobRing_finish(self);
    Py_RETURN_NONE;
}

PyDoc_STRVAR(BlobRing_allocated__doc__,
  "allocated() -> int\n"
  "\n"
  "Bytes currently allocated for slots.");

static PyObject *
BlobRing_allocated(BlobRing *self, PyObject *Py_UNUSED(ignored))
{
    size_t i, total = 0;

    CHECK_RING_INIT(self);
    for (i = 0; i < self->nslots; i++)
        total += self->slot[i] != NULL ? self->cap[i] : 0;
    return PyLong_FromSize_t(total);
}

static PyMethodDef BlobRing_methods[] = {
    {"readinto", (PyCFunction)BlobRing_readinto, METH_O, BlobRing_readinto__doc__},
    {"close", (PyCFunction)BlobRing_close, METH_NOARGS, BlobRing_close__doc__},
    {"close_write", (PyCFunction)BlobRing_close_write, METH_NOARGS, BlobRing_close_write__doc__},
    {"allocated", (PyCFunction)BlobRing_allocated, METH_NOARGS, BlobRing_allocated__doc__},
    {NULL}
};

PyDoc_STRVAR(BlobRing__doc__,
  "_BlobRing(slots: int, slot_size: int, size_hint: int = 0)\n"
  "\n"
  "Internal buffer between BlobIO's writer thread and its reader. Written\n"
  "with Blob._write_to_ring, read with readinto. One writer, one reader.");

PyTypeObject BlobRingType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "_pygit2._BlobRing",                       /* tp_name           */
    sizeof(BlobRing),                          /* tp_basicsize      */
    0,                                         /* tp_itemsize       */
    (destructor)BlobRing_dealloc,              /* tp_dealloc        */
    0,                                         /* tp_print          */
    0,                                         /* tp_getattr        */
    0,                                         /* tp_setattr        */
    0,                                         /* tp_compare        */
    0,                                         /* tp_repr           */
    0,                                         /* tp_as_number      */
    0,                                         /* tp_as_sequence    */
    0,                                         /* tp_as_mapping     */
    0,                                         /* tp_hash           */
    0,                                         /* tp_call           */
    0,                                         /* tp_str            */
    0,                                         /* tp_getattro       */
    0,                                         /* tp_setattro       */
    0,                                         /* tp_as_buffer      */
    Py_TPFLAGS_DEFAULT,                        /* tp_flags          */
    BlobRing__doc__,                           /* tp_doc            */
    0,                                         /* tp_traverse       */
    0,                                         /* tp_clear          */
    0,                                         /* tp_richcompare    */
    0,                                         /* tp_weaklistoffset */
    0,                                         /* tp_iter           */
    0,                                         /* tp_iternext       */
    BlobRing_methods,                          /* tp_methods        */
    0,                                         /* tp_members        */
    0,                                         /* tp_getset         */
    0,                                         /* tp_base           */
    0,                                         /* tp_dict           */
    0,                                         /* tp_descr_get      */
    0,                                         /* tp_descr_set      */
    0,                                         /* tp_dictoffset     */
    (initproc)BlobRing_init,                   /* tp_init           */
    0,                                         /* tp_alloc          */
    0,                                         /* tp_new            */
};
