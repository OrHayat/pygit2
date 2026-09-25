import io
import threading
from contextlib import AbstractContextManager
from typing import Optional

from ._pygit2 import Blob, Oid, _BlobRing
from .enums import BlobFilter

# libgit2's output is batched into slots of RING_SLOT_SIZE bytes; the ring holds
# up to RING_SLOTS of them. Slots are allocated lazily and the first one is
# sized from the blob, so small blobs use little memory.
RING_SLOTS = 4
RING_SLOT_SIZE = 128 * 1024


class _BlobIO(io.RawIOBase):
    """Low-level wrapper for streaming blob content.

    The underlying libgit2 git_writestream filter chain runs in a separate
    thread and writes into a _BlobRing, with the GIL released. Closing
    before the end stops libgit2 instead of streaming the rest.
    """

    def __init__(
        self,
        blob: Blob,
        as_path: Optional[str] = None,
        flags: BlobFilter = BlobFilter.CHECK_FOR_BINARY,
        commit_id: Optional[Oid] = None,
    ):
        super().__init__()
        self._blob = blob
        self._ring = _BlobRing(RING_SLOTS, RING_SLOT_SIZE)
        self._error: Optional[BaseException] = None
        self._thread = threading.Thread(
            target=self._write,
            args=(as_path, int(flags), commit_id),
            daemon=True,
        )
        self._thread.start()

    def _write(
        self, as_path: Optional[str], flags: int, commit_id: Optional[Oid]
    ) -> None:
        try:
            self._blob._write_to_ring(
                self._ring, as_path=as_path, flags=flags, commit_id=commit_id
            )
        except BaseException as e:
            # Raised by readinto(); otherwise the reader would just see EOF
            self._error = e
        finally:
            # _write_to_ring signals the end itself; this also covers errors
            # raised before it got that far
            self._ring.close_write()

    def __exit__(self, exc_type, exc_value, traceback):
        self.close()

    def isatty(self):
        return False

    def readable(self):
        return True

    def writable(self):
        return False

    def seekable(self):
        return False

    def readinto(self, b, /):
        try:
            n = self._ring.readinto(b)
        except KeyboardInterrupt:
            return 0
        if n == 0 and self._error is not None:
            error, self._error = self._error, None
            raise error
        return n

    def close(self) -> None:
        if self.closed:
            return
        try:
            self._ring.close()
            self._thread.join()
        except KeyboardInterrupt:
            pass
        super().close()


class BlobIO(io.BufferedReader, AbstractContextManager):
    """Read-only wrapper for streaming blob content.

    Supports reading both raw and filtered blob content.
    Implements io.BufferedReader.

    Example:

        >>> with BlobIO(blob) as f:
        ...     while True:
        ...         # Read blob data in 1KB chunks until EOF is reached
        ...         chunk = f.read(1024)
        ...         if not chunk:
        ...             break

    By default, `BlobIO` will stream the raw contents of the blob, but it
    can also be used to stream filtered content (i.e. to read the content
    after applying filters which would be used when checking out the blob
    to the working directory).

    Example:

        >>> with BlobIO(blob, as_path='my_file.ext') as f:
        ...     # Read the filtered content which would be returned upon
        ...     # running 'git checkout -- my_file.txt'
        ...     filtered_data = f.read()
    """

    def __init__(
        self,
        blob: Blob,
        as_path: Optional[str] = None,
        flags: BlobFilter = BlobFilter.CHECK_FOR_BINARY,
        commit_id: Optional[Oid] = None,
    ):
        """Wrap the specified blob.

        Parameters:
            blob: The blob to wrap.
            as_path: Filter the contents of the blob as if it had the specified
                path. If `as_path` is None, the raw contents of the blob will
                be read.
            flags: A combination of enums.BlobFilter constants
                (only applicable when `as_path` is set).
            commit_id: Commit to load attributes from when
                ATTRIBUTES_FROM_COMMIT is specified in `flags`
                (only applicable when `as_path` is set).
        """
        raw = _BlobIO(blob, as_path=as_path, flags=flags, commit_id=commit_id)
        super().__init__(raw)

    def __exit__(self, exc_type, exc_value, traceback):
        self.close()


io.RawIOBase.register(_BlobIO)
io.BufferedIOBase.register(BlobIO)
