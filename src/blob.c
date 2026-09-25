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

#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <git2.h>
#include <git2/sys/errors.h>
#include "blobio.h"
#include "diff.h"
#include "error.h"
#include "object.h"
#include "oid.h"
#include "patch.h"
#include "utils.h"

extern PyObject *GitError;

extern PyTypeObject BlobType;

PyDoc_STRVAR(Blob_diff__doc__,
  "diff([blob: Blob, flags: int = GIT_DIFF_NORMAL, old_as_path: str, new_as_path: str]) -> Patch\n"
  "\n"
  "Directly generate a :py:class:`pygit2.Patch` from the difference\n"
  "between two blobs.\n"
  "\n"
  "Returns: Patch.\n"
  "\n"
  "Parameters:\n"
  "\n"
  "blob : Blob\n"
  "    The :py:class:`~pygit2.Blob` to diff.\n"
  "\n"
  "flags\n"
  "    A combination of GIT_DIFF_* constant.\n"
  "\n"
  "old_as_path : str\n"
  "    Treat old blob as if it had this filename.\n"
  "\n"
  "new_as_path : str\n"
  "    Treat new blob as if it had this filename.\n"
  "\n"
  "context_lines: int\n"
  "    Number of unchanged lines that define the boundary of a hunk\n"
  "    (and to display before and after).\n"
  "\n"
  "interhunk_lines: int\n"
  "    Maximum number of unchanged lines between hunk boundaries\n"
  "    before the hunks will be merged into one.\n");

PyObject *
Blob_diff(Blob *self, PyObject *args, PyObject *kwds)
{
    git_diff_options opts = GIT_DIFF_OPTIONS_INIT;
    git_patch *patch;
    char *old_as_path = NULL, *new_as_path = NULL;
    Blob *other = NULL;
    int err;
    char *keywords[] = {"blob", "flags", "old_as_path", "new_as_path", "context_lines", "interhunk_lines", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|O!IssHH", keywords,
                                     &BlobType, &other, &opts.flags,
                                     &old_as_path, &new_as_path,
                                     &opts.context_lines, &opts.interhunk_lines))
        return NULL;

    if (Object__load((Object*)self) == NULL) { return NULL; } // Lazy load
    if (other && Object__load((Object*)other) == NULL) { return NULL; } // Lazy load

    err = git_patch_from_blobs(&patch, self->blob, old_as_path,
                               other ? other->blob : NULL, new_as_path,
                               &opts);
    if (err < 0)
        return Error_set(err);

    return wrap_patch(patch, self, other);
}


PyDoc_STRVAR(Blob_diff_to_buffer__doc__,
  "diff_to_buffer(buffer: bytes = None, flags: int = GIT_DIFF_NORMAL[, old_as_path: str, buffer_as_path: str]) -> Patch\n"
  "\n"
  "Directly generate a :py:class:`~pygit2.Patch` from the difference\n"
  "between a blob and a buffer.\n"
  "\n"
  "Returns: Patch.\n"
  "\n"
  "Parameters:\n"
  "\n"
  "buffer : bytes\n"
  "    Raw data for new side of diff.\n"
  "\n"
  "flags\n"
  "    A combination of GIT_DIFF_* constants.\n"
  "\n"
  "old_as_path : str\n"
  "    Treat old blob as if it had this filename.\n"
  "\n"
  "buffer_as_path : str\n"
  "    Treat buffer as if it had this filename.\n");

PyObject *
Blob_diff_to_buffer(Blob *self, PyObject *args, PyObject *kwds)
{
    git_diff_options opts = GIT_DIFF_OPTIONS_INIT;
    git_patch *patch;
    char *old_as_path = NULL, *buffer_as_path = NULL;
    const char *buffer = NULL;
    Py_ssize_t buffer_len;
    int err;
    char *keywords[] = {"buffer", "flags", "old_as_path", "buffer_as_path", NULL};

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|z#Iss", keywords,
                                     &buffer, &buffer_len, &opts.flags,
                                     &old_as_path, &buffer_as_path))
        return NULL;

    if (Object__load((Object*)self) == NULL) { return NULL; } // Lazy load

    err = git_patch_from_blob_and_buffer(&patch, self->blob, old_as_path,
                                         buffer, buffer_len, buffer_as_path,
                                         &opts);
    if (err < 0)
        return Error_set(err);

    return wrap_patch(patch, self, NULL);
}


PyDoc_STRVAR(Blob__write_to_ring__doc__,
  "_write_to_ring(ring: _BlobRing, [as_path: str = None, flags: enums.BlobFilter = enums.BlobFilter.CHECK_FOR_BINARY, commit_id: oid = None]) -> None\n"
  "\n"
  "Stream the contents of the blob into `ring`.\n"
  "If `as_path` is None, the raw contents of the blob are written,\n"
  "otherwise the contents of the blob are filtered.\n"
  "\n"
  "In most cases the higher level `BlobIO` wrapper should be used instead\n"
  "of calling this method directly.\n"
  "\n"
  "This method blocks the current thread until the whole blob has been\n"
  "written, or until the reader closes the ring. The GIL is released while\n"
  "libgit2 streams the blob. The end of the stream is always signalled to\n"
  "the reader, also on errors.\n"
  "\n"
  "Parameters:\n"
  "\n"
  "ring : _BlobRing\n"
  "    Destination ring.\n"
  "\n"
  "as_path : str\n"
  "    When set, the blob contents will be filtered as if it had this\n"
  "    filename (used for attribute lookups).\n"
  "\n"
  "flags : enums.BlobFilter\n"
  "    A combination of BlobFilter constants (only applicable when `as_path` is set).\n"
  "\n"
  "commit_id : oid\n"
  "    Commit to load attributes from when ATTRIBUTES_FROM_COMMIT is\n"
  "    specified in `flags` (only applicable when `as_path` is set).\n");

PyObject *
Blob__write_to_ring(Blob *self, PyObject *args, PyObject *kwds)
{
    PyObject *py_ring = NULL;
    BlobRing *ring;
    char *as_path = NULL;
    PyObject *py_oid = NULL;
    PyObject *result = NULL;
    int err;
    char *keywords[] = {"ring", "as_path", "flags", "commit_id", NULL};
    git_blob_filter_options opts = GIT_BLOB_FILTER_OPTIONS_INIT;
    git_filter_options filter_opts = GIT_FILTER_OPTIONS_INIT;
    git_filter_list *fl = NULL;
    git_blob *blob = NULL;
    const git_oid *blob_oid;

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "O!|zIO", keywords,
                                     &BlobRingType, &py_ring, &as_path,
                                     &opts.flags, &py_oid))
        return NULL;
    ring = (BlobRing *)py_ring;

    /* From here on every exit goes through `done`, which signals the end of
     * the stream so the reader never waits forever. */
    if (Object__load((Object*)self) == NULL)  // Lazy load
        goto done;

    /* we load our own copy of this blob since libgit2 objects are not
     * thread-safe */
    blob_oid = Object__id((Object*)self);
    err = git_blob_lookup(&blob, git_blob_owner(self->blob), blob_oid);
    if (err < 0) {
        Error_set(err);
        goto done;
    }

    if (as_path != NULL &&
        !((opts.flags & GIT_BLOB_FILTER_CHECK_FOR_BINARY) != 0 &&
          git_blob_is_binary(blob)))
    {
        if (py_oid != NULL && py_oid != Py_None)
        {
            size_t len = py_oid_to_git_oid(py_oid, &opts.attr_commit_id);
            if (len == 0)
                goto done;
        }

        if ((opts.flags & GIT_BLOB_FILTER_NO_SYSTEM_ATTRIBUTES) != 0)
            filter_opts.flags |= GIT_FILTER_NO_SYSTEM_ATTRIBUTES;
        if ((opts.flags & GIT_BLOB_FILTER_ATTRIBUTES_FROM_HEAD) != 0)
            filter_opts.flags |= GIT_FILTER_ATTRIBUTES_FROM_HEAD;
        if ((opts.flags & GIT_BLOB_FILTER_ATTRIBUTES_FROM_COMMIT) != 0)
            filter_opts.flags |= GIT_FILTER_ATTRIBUTES_FROM_COMMIT;
        git_oid_cpy(&filter_opts.attr_commit_id, &opts.attr_commit_id);

        err = git_filter_list_load_ext(&fl, git_blob_owner(blob), blob,
                                       as_path, GIT_FILTER_TO_WORKTREE,
                                       &filter_opts);
        if (err < 0) {
            Error_set(err);
            goto done;
        }
    }

    BlobRing_set_size_hint(ring, (size_t)git_blob_rawsize(blob));

    Py_BEGIN_ALLOW_THREADS;
    err = BlobRing_stream(ring, fl, blob);
    Py_END_ALLOW_THREADS;

    /* A failed write after the reader closed the ring is not an error */
    if (err < 0 && !BlobRing_cancelled(ring))
        Error_set(err);
    else
        result = Py_NewRef(Py_None);

done:
    git_filter_list_free(fl);
    git_blob_free(blob);
    BlobRing_finish(ring);
    return result;
}

static PyMethodDef Blob_methods[] = {
    METHOD(Blob, diff, METH_VARARGS | METH_KEYWORDS),
    METHOD(Blob, diff_to_buffer, METH_VARARGS | METH_KEYWORDS),
    METHOD(Blob, _write_to_ring, METH_VARARGS | METH_KEYWORDS),
    {NULL}
};


PyDoc_STRVAR(Blob_size__doc__,
    "Size in bytes.\n"
    "\n"
    "Example:\n"
    "\n"
    "    >>> print(blob.size)\n"
    "    130\n");

PyObject *
Blob_size__get__(Blob *self)
{
    if (Object__load((Object*)self) == NULL) { return NULL; } // Lazy load
    return PyLong_FromLongLong(git_blob_rawsize(self->blob));
}


PyDoc_STRVAR(Blob_is_binary__doc__, "True if binary data, False if not.");

PyObject *
Blob_is_binary__get__(Blob *self)
{
    if (Object__load((Object*)self) == NULL) { return NULL; } // Lazy load

    if (git_blob_is_binary(self->blob))
        Py_RETURN_TRUE;
    Py_RETURN_FALSE;
}


PyDoc_STRVAR(Blob_data__doc__,
    "The contents of the blob, a byte string. This is the same as\n"
    "Blob.read_raw().\n"
    "\n"
    "Example, print the contents of the ``.gitignore`` file:\n"
    "\n"
    "    >>> blob = repo['d8022420bf6db02e906175f64f66676df539f2fd']\n"
    "    >>> print(blob.data)\n"
    "    MANIFEST\n"
    "    build\n"
    "    dist\n");

PyGetSetDef Blob_getseters[] = {
    GETTER(Blob, size),
    GETTER(Blob, is_binary),
    {"data", (getter)Object_read_raw, NULL, Blob_data__doc__, NULL},
    {NULL}
};

static int
Blob_getbuffer(Blob *self, Py_buffer *view, int flags)
{
    if (Object__load((Object*)self) == NULL) { return -1; } // Lazy load
    return PyBuffer_FillInfo(view, (PyObject *) self,
                             (void *) git_blob_rawcontent(self->blob),
                             git_blob_rawsize(self->blob), 1, flags);
}

static PyBufferProcs Blob_as_buffer = {
    (getbufferproc)Blob_getbuffer,
};

PyDoc_STRVAR(Blob__doc__, "Blob object.\n"
  "\n"
  "Blobs implement the buffer interface, which means you can get access\n"
  "to its data via `memoryview(blob)` without the need to create a copy."
);

PyTypeObject BlobType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    "_pygit2.Blob",                            /* tp_name           */
    sizeof(Blob),                              /* tp_basicsize      */
    0,                                         /* tp_itemsize       */
    0,                                         /* tp_dealloc        */
    0,                                         /* tp_print          */
    0,                                         /* tp_getattr        */
    0,                                         /* tp_setattr        */
    0,                                         /* tp_compare        */
    (reprfunc)Object_repr,                     /* tp_repr           */
    0,                                         /* tp_as_number      */
    0,                                         /* tp_as_sequence    */
    0,                                         /* tp_as_mapping     */
    0,                                         /* tp_hash           */
    0,                                         /* tp_call           */
    0,                                         /* tp_str            */
    0,                                         /* tp_getattro       */
    0,                                         /* tp_setattro       */
    &Blob_as_buffer,                           /* tp_as_buffer      */
    Py_TPFLAGS_DEFAULT,                        /* tp_flags          */
    Blob__doc__,                               /* tp_doc            */
    0,                                         /* tp_traverse       */
    0,                                         /* tp_clear          */
    0,                                         /* tp_richcompare    */
    0,                                         /* tp_weaklistoffset */
    0,                                         /* tp_iter           */
    0,                                         /* tp_iternext       */
    Blob_methods,                              /* tp_methods        */
    0,                                         /* tp_members        */
    Blob_getseters,                            /* tp_getset         */
    0,                                         /* tp_base           */
    0,                                         /* tp_dict           */
    0,                                         /* tp_descr_get      */
    0,                                         /* tp_descr_set      */
    0,                                         /* tp_dictoffset     */
    0,                                         /* tp_init           */
    0,                                         /* tp_alloc          */
    0,                                         /* tp_new            */
};
