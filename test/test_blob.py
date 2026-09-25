# Copyright 2010-2026 The pygit2 contributors
#
# This file is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 2,
# as published by the Free Software Foundation.
#
# In addition to the permissions in the GNU General Public License,
# the authors give you unlimited permission to link the compiled
# version of this file into combinations with other programs,
# and to distribute those combinations without any restriction
# coming from the use of this file.  (The General Public License
# restrictions do apply in other respects; for example, they cover
# modification of the file, and distribution when not linked into
# a combined executable.)
#
# This file is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; see the file COPYING.  If not, write to
# the Free Software Foundation, 51 Franklin Street, Fifth Floor,
# Boston, MA 02110-1301, USA.

"""Tests for Blob objects."""

import io
import threading
from collections.abc import Callable
from pathlib import Path

import pytest

import pygit2
from pygit2 import Repository
from pygit2.enums import BlobFilter, ObjectType

from . import utils

BLOB_SHA = 'a520c24d85fbfc815d385957eed41406ca5a860b'
BLOB_CONTENT = """hello world
hola mundo
bonjour le monde
""".encode()
BLOB_NEW_CONTENT = b'foo bar\n'
BLOB_FILE_CONTENT = b'bye world\n'

BLOB_PATCH = r"""diff --git a/file b/file
index a520c24..95d09f2 100644
--- a/file
+++ b/file
@@ -1,3 +1 @@
-hello world
-hola mundo
-bonjour le monde
+hello world
\ No newline at end of file
"""

BLOB_PATCH_2 = """diff --git a/file b/file
index a520c24..d675fa4 100644
--- a/file
+++ b/file
@@ -1,3 +1 @@
-hello world
-hola mundo
-bonjour le monde
+foo bar
"""

BLOB_PATCH_DELETED = """diff --git a/file b/file
deleted file mode 100644
index a520c24..0000000
--- a/file
+++ /dev/null
@@ -1,3 +0,0 @@
-hello world
-hola mundo
-bonjour le monde
"""


def test_read_blob(testrepo: Repository) -> None:
    blob = testrepo[BLOB_SHA]
    assert blob.id == BLOB_SHA
    assert blob.id == BLOB_SHA
    assert isinstance(blob, pygit2.Blob)
    assert not blob.is_binary
    assert ObjectType.BLOB == blob.type
    assert BLOB_CONTENT == blob.data
    assert len(BLOB_CONTENT) == blob.size
    assert BLOB_CONTENT == blob.read_raw()


def test_create_blob(testrepo: Repository) -> None:
    blob_oid = testrepo.create_blob(BLOB_NEW_CONTENT)
    blob = testrepo[blob_oid]

    assert isinstance(blob, pygit2.Blob)
    assert ObjectType.BLOB == blob.type

    assert blob_oid == blob.id
    assert utils.gen_blob_sha1(BLOB_NEW_CONTENT) == blob_oid

    assert BLOB_NEW_CONTENT == blob.data
    assert len(BLOB_NEW_CONTENT) == blob.size
    assert BLOB_NEW_CONTENT == blob.read_raw()
    blob_buffer = memoryview(blob)
    assert len(BLOB_NEW_CONTENT) == len(blob_buffer)
    assert BLOB_NEW_CONTENT == blob_buffer

    def set_content() -> None:
        blob_buffer[:2] = b'hi'

    with pytest.raises(TypeError):
        set_content()


def test_create_blob_fromworkdir(testrepo: Repository) -> None:
    blob_oid = testrepo.create_blob_fromworkdir('bye.txt')
    blob = testrepo[blob_oid]

    assert isinstance(blob, pygit2.Blob)
    assert ObjectType.BLOB == blob.type

    assert blob_oid == blob.id
    assert utils.gen_blob_sha1(BLOB_FILE_CONTENT) == blob_oid

    assert BLOB_FILE_CONTENT == blob.data
    assert len(BLOB_FILE_CONTENT) == blob.size
    assert BLOB_FILE_CONTENT == blob.read_raw()


def test_create_blob_fromworkdir_aspath(testrepo: Repository) -> None:
    blob_oid = testrepo.create_blob_fromworkdir(Path('bye.txt'))
    blob = testrepo[blob_oid]

    assert isinstance(blob, pygit2.Blob)


def test_create_blob_outside_workdir(testrepo: Repository) -> None:
    with pytest.raises(KeyError):
        testrepo.create_blob_fromworkdir(__file__)


def test_create_blob_fromdisk(testrepo: Repository) -> None:
    blob_oid = testrepo.create_blob_fromdisk(__file__)
    blob = testrepo[blob_oid]

    assert isinstance(blob, pygit2.Blob)
    assert ObjectType.BLOB == blob.type


def test_create_blob_fromiobase(testrepo: Repository) -> None:
    with pytest.raises(TypeError):
        testrepo.create_blob_fromiobase('bad type')  # type: ignore

    f = io.BytesIO(BLOB_CONTENT)
    blob_oid = testrepo.create_blob_fromiobase(f)
    blob = testrepo[blob_oid]

    assert isinstance(blob, pygit2.Blob)
    assert ObjectType.BLOB == blob.type

    assert blob_oid == blob.id
    assert BLOB_SHA == blob_oid


def test_diff_blob(testrepo: Repository) -> None:
    blob = testrepo[BLOB_SHA]
    assert isinstance(blob, pygit2.Blob)
    old_blob = testrepo['3b18e512dba79e4c8300dd08aeb37f8e728b8dad']
    assert isinstance(old_blob, pygit2.Blob)
    patch = blob.diff(old_blob, old_as_path='hello.txt')
    assert len(patch.hunks) == 1


def test_diff_blob_to_buffer(testrepo: Repository) -> None:
    blob = testrepo[BLOB_SHA]
    assert isinstance(blob, pygit2.Blob)
    patch = blob.diff_to_buffer('hello world')
    assert len(patch.hunks) == 1


def test_diff_blob_to_buffer_patch_patch(testrepo: Repository) -> None:
    blob = testrepo[BLOB_SHA]
    assert isinstance(blob, pygit2.Blob)
    patch = blob.diff_to_buffer('hello world')
    assert patch.text == BLOB_PATCH


def test_diff_blob_to_buffer_delete(testrepo: Repository) -> None:
    blob = testrepo[BLOB_SHA]
    assert isinstance(blob, pygit2.Blob)
    patch = blob.diff_to_buffer(None)
    assert patch.text == BLOB_PATCH_DELETED


def test_diff_blob_create(testrepo: Repository) -> None:
    old = testrepo[testrepo.create_blob(BLOB_CONTENT)]
    new = testrepo[testrepo.create_blob(BLOB_NEW_CONTENT)]
    assert isinstance(old, pygit2.Blob)
    assert isinstance(new, pygit2.Blob)

    patch = old.diff(new)
    assert patch.text == BLOB_PATCH_2


def test_blob_from_repo(testrepo: Repository) -> None:
    blob = testrepo[BLOB_SHA]
    assert isinstance(blob, pygit2.Blob)
    patch_one = blob.diff_to_buffer(None)

    blob = testrepo[BLOB_SHA]
    assert isinstance(blob, pygit2.Blob)
    patch_two = blob.diff_to_buffer(None)

    assert patch_one.text == patch_two.text


def read_ring(ring: pygit2._pygit2._BlobRing) -> bytes:
    chunks = []
    buf = bytearray(64 * 1024)
    while n := ring.readinto(buf):
        chunks.append(bytes(buf[:n]))
    return b''.join(chunks)


def test_blob_write_to_ring(testrepo: Repository) -> None:
    ring = pygit2._pygit2._BlobRing(4, 128 * 1024)
    blob = testrepo[BLOB_SHA]
    assert isinstance(blob, pygit2.Blob)
    blob._write_to_ring(ring)
    assert BLOB_CONTENT == read_ring(ring)


def test_blob_write_to_ring_filtered(testrepo: Repository) -> None:
    ring = pygit2._pygit2._BlobRing(4, 128 * 1024)
    blob_oid = testrepo.create_blob_fromworkdir('bye.txt')
    blob = testrepo[blob_oid]
    assert isinstance(blob, pygit2.Blob)
    blob._write_to_ring(ring, as_path='bye.txt')
    assert b'bye world\n' == read_ring(ring)


def test_blob_write_to_ring_small_slot(testrepo: Repository) -> None:
    # Output larger than a slot spans several slots
    ring = pygit2._pygit2._BlobRing(4, 16)
    data = bytes(range(256)) * 4
    blob = testrepo[testrepo.create_blob(data)]
    assert isinstance(blob, pygit2.Blob)
    thread = threading.Thread(target=blob._write_to_ring, args=(ring,))
    thread.start()
    assert data == read_ring(ring)
    thread.join()


def test_blob_write_to_ring_lazy_allocation(testrepo: Repository) -> None:
    # The first slot is sized from the blob, so small blobs use little memory
    ring = pygit2._pygit2._BlobRing(4, 128 * 1024)
    assert ring.allocated() == 0
    blob = testrepo[BLOB_SHA]
    assert isinstance(blob, pygit2.Blob)
    blob._write_to_ring(ring)
    assert 0 < ring.allocated() <= 4096
    assert BLOB_CONTENT == read_ring(ring)


def test_blob_write_to_ring_invalid_commit_id_type(testrepo: Repository) -> None:
    # Regression test (issue #1478): an invalid commit_id type must raise
    # TypeError instead of being ignored and leaving an exception set.
    ring = pygit2._pygit2._BlobRing(4, 128 * 1024)
    blob_oid = testrepo.create_blob_fromworkdir('bye.txt')
    blob = testrepo[blob_oid]
    assert isinstance(blob, pygit2.Blob)
    with pytest.raises(TypeError):
        blob._write_to_ring(
            ring,
            as_path='bye.txt',
            flags=BlobFilter.ATTRIBUTES_FROM_COMMIT,
            commit_id=1234,  # type: ignore
        )
    # The end of the stream is signalled even on errors
    assert read_ring(ring) == b''


def test_blob_write_to_ring_invalid_commit_id_str(testrepo: Repository) -> None:
    # Regression test (issue #1478): a malformed commit_id string must raise
    # InvalidError instead of being ignored and leaving an exception set.
    ring = pygit2._pygit2._BlobRing(4, 128 * 1024)
    blob_oid = testrepo.create_blob_fromworkdir('bye.txt')
    blob = testrepo[blob_oid]
    assert isinstance(blob, pygit2.Blob)
    with pytest.raises(pygit2.InvalidError):
        blob._write_to_ring(
            ring,
            as_path='bye.txt',
            flags=BlobFilter.ATTRIBUTES_FROM_COMMIT,
            commit_id='not-a-valid-oid',  # type: ignore[arg-type]
        )
    assert read_ring(ring) == b''


def test_blobio(testrepo: Repository) -> None:
    blob_oid = testrepo.create_blob_fromworkdir('bye.txt')
    blob = testrepo[blob_oid]
    assert isinstance(blob, pygit2.Blob)
    with pygit2.BlobIO(blob) as reader:
        assert b'bye world\n' == reader.read()
    assert not reader.raw._thread.is_alive()  # type: ignore[attr-defined]


def test_blobio_filtered(testrepo: Repository) -> None:
    blob_oid = testrepo.create_blob_fromworkdir('bye.txt')
    blob = testrepo[blob_oid]
    assert isinstance(blob, pygit2.Blob)
    with pygit2.BlobIO(blob, as_path='bye.txt') as reader:
        assert b'bye world\n' == reader.read()
    assert not reader.raw._thread.is_alive()  # type: ignore[attr-defined]


def test_blob_partial_read(bigrepo: Repository) -> None:
    blob_oid = bigrepo.create_blob_fromworkdir('big.txt')
    blob = bigrepo[blob_oid]
    assert isinstance(blob, pygit2.Blob)
    reader = pygit2.BlobIO(blob)
    # Read only a few lines then break early
    for i, line in enumerate(reader):
        if i >= 3:
            break
    reader.close()
    assert not reader.raw._thread.is_alive()  # type: ignore[attr-defined]


def run_with_timeout(fn: Callable[[], None], timeout: float = 30) -> None:
    """Run fn in a thread; fail instead of hanging the test suite."""
    errors: list[BaseException] = []

    def target() -> None:
        try:
            fn()
        except BaseException as e:
            errors.append(e)

    thread = threading.Thread(target=target, daemon=True)
    thread.start()
    thread.join(timeout)
    assert not thread.is_alive(), 'BlobIO hung'
    if errors:
        raise errors[0]


def test_blobio_writer_error(testrepo: Repository) -> None:
    # An error in the writer thread is raised by read() instead of hanging
    blob_oid = testrepo.create_blob_fromworkdir('bye.txt')
    blob = testrepo[blob_oid]
    assert isinstance(blob, pygit2.Blob)

    def read() -> None:
        with pygit2.BlobIO(
            blob,
            as_path='bye.txt',
            flags=BlobFilter.ATTRIBUTES_FROM_COMMIT,
            commit_id='not-a-valid-oid',  # type: ignore[arg-type]
        ) as reader:
            with pytest.raises(pygit2.InvalidError):
                reader.read()

    run_with_timeout(read)


def test_blobio_partial_read_stress(testrepo: Repository) -> None:
    # Regression test: close() after a partial read used to hang now and then
    # (lost wakeup between the reader and the writer thread)
    data = b''.join(b'line %d\n' % i for i in range(20000))
    blob = testrepo[testrepo.create_blob(data)]
    assert isinstance(blob, pygit2.Blob)

    def read() -> None:
        for _ in range(500):
            reader = pygit2.BlobIO(blob)
            for i, line in enumerate(reader):
                if i >= 3:
                    break
            reader.close()
            assert not reader.raw._thread.is_alive()  # type: ignore[attr-defined]
            with pygit2.BlobIO(blob) as reader:
                assert reader.read() == data

    run_with_timeout(read, timeout=120)
