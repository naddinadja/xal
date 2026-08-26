# Pool Memory

Inodes and extents are each stored in a separate pool (``struct xal_pool``),
backed by a large over-committed ``mmap`` region.

The pool reserves a virtual address range upfront sized for the maximum
expected number of elements, but only commits physical pages in chunks as
elements are claimed (via ``mprotect``). This keeps the array contiguous in
memory — ``xal_inode_at(xal, idx)`` is a plain pointer offset — and means
elements never move, so pool indices remain stable across all insertions.

## Lazy growth (anonymous mode)

By default, pools use private anonymous memory. The full virtual address range
is reserved with ``PROT_NONE`` at open time; physical pages are committed in
chunks of ``growby`` elements by calling ``mprotect(PROT_READ|PROT_WRITE)``
whenever the pool runs low. This avoids upfront memory commitment while keeping
the array at a single contiguous address.

## Shared memory mode

When ``xal_opts.shm_name`` is set, the two pools are backed by POSIX shared
memory objects instead of anonymous memory. Because all internal
cross-references within the pools use integer indices rather than raw
pointers, the pool data is valid regardless of the virtual address at which
it is mapped in each process. The names of the objects are
derived from the base name by appending ``_inodes`` and ``_extents``
respectively::

   opts.shm_name = "/myapp_xal";
   /* creates /myapp_xal_inodes and /myapp_xal_extents */

In this mode the full reserved size is committed upfront via ``ftruncate()``
and ``mmap(MAP_SHARED)``; there is no lazy growth. The objects live in the
shared memory filesystem (``/dev/shm`` on Linux) and are removed by the
creating process on ``xal_close()``; see "Process roles" below.

## Consumer processes: ``xal_from_shm()``

A secondary process that needs read-only access to an already-indexed pool can
attach to the shared memory objects directly, without opening the device or
re-running ``xal_index()``. All metadata (superblock, backend type, mountpoint,
root inode index) is read from a dedicated ``_state`` shared memory region
created by the primary; no out-of-band communication is needed beyond the base
shared memory name.

The typical pattern is:

1. One process calls ``xal_open()`` with ``shm_name`` set and runs
   ``xal_index()``. The shared memory name must be communicated to the
   secondary process, for example through a command-line argument or
   environment variable.

2. The secondary calls ``xal_from_shm()`` with that name to obtain a
   read-only ``struct xal *``::

      const char *shm_name = /* shared memory base name */;
      struct xal *view;

      xal_from_shm(shm_name, &view);
      xal_walk(view, xal_get_root(view), my_callback, NULL);
      xal_close(view);

## Symmetric processes: ``xal_open()``

Step 2 above requires each process to know in advance whether it is the one
building the index. When that is not known — peers started in any order, a
restart racing its own replacement — ``xal_open()`` with ``shm_name`` set
decides it instead: if no index is published under the name, this process
creates the regions and becomes the primary; if one is, it attaches to it
exactly as ``xal_from_shm()`` would, without touching the device. Every
process can then run the same code::

   struct xal_opts opts = {.shm_name = "/myapp_xal"};
   struct xal *xal;

   err = xal_open(dev, &xal, &opts);
   if (err) {
        return err;  // -EAGAIN, -ESTALE and -EEXIST mean "not yet", see below
   }

   if (xal_get_procrole(xal) == XAL_PROCROLE_PRIMARY) {
        xal_dinodes_retrieve(xal);  // XFS backend only; a no-op under FIEMAP
        xal_index(xal);
   }

### Losing the race is not a failure

A process that arrives while the primary is still working does not get a
handle, and three errno values say so rather than reporting a real fault:

``-EAGAIN``
   The ``_state`` region exists but is not published. The owner is either
   between creating it and filling it in, or on its way out and dismantling
   the regions behind it.

``-ESTALE``
   The region is published but the index is not built, or no longer matches
   the filesystem. This covers the whole span from ``xal_open()`` returning
   in the primary to its ``xal_index()`` completing, and any later period
   where the primary has marked the index dirty.

``-EEXIST``
   Two processes both found the name free and this one lost the race to
   create the pools.

All three mean the same thing to a caller: wait and open again. Anything else
is a genuine failure.

Two rules produce this. Within the ``_state`` region a ``ready`` marker is
written last, after every other field, and read first — so a region is only
attachable once it is complete, and the index it describes is marked dirty
from creation until an ``xal_index()`` finishes. Around it, ``_state`` is
unlinked *after* the pools it describes and cleared of ``ready`` before them,
so a name that looks free really is free: no peer sees an unused name whose
pools are still lingering, and none attaches to pools being unmapped
underneath it.

### Recovering after a crash

Everything above concerns processes that close in an orderly way. A primary
that is killed runs none of it, and the regions it created outlive it. Since
the pools are created with ``O_EXCL``, that debris blocks the name:

- ``_state`` left published: the next ``xal_open()`` attaches to whatever the
  dead primary last wrote. It looks like a healthy index, but describes a
  filesystem nobody is watching any more.
- ``_state`` left unpublished, or left behind mid-teardown: every later
  ``xal_open()`` gets ``-EAGAIN`` and retries forever.
- Only the pools left: every later ``xal_open()`` fails with ``-EEXIST`` from
  ``xal_pool_map()``.

None of these resolve on their own, and no amount of ordering fixes them —
ordering only helps a process that is alive to run it. Clearing the debris is
currently manual::

   rm -f /dev/shm/myapp_xal_state /dev/shm/myapp_xal_inodes /dev/shm/myapp_xal_extents

A starting process cannot do that on its own today, because a stale region and
a live owner's region look identical. Distinguishing them needs a signal the
kernel maintains on the owner's behalf, which is the intended fix: have the
primary take an ``flock(LOCK_EX)`` on the ``_state`` descriptor and hold it
for its lifetime. The kernel releases that lock however the process dies, so a
peer that acquires it with ``LOCK_NB`` has proven there is no live owner, and
can unlink whatever regions remain under the name before creating its own.
Peers that fail to acquire it have equally proven there *is* one, and attach
as usual. The unlink ordering described above is what makes that safe to add:
``_state`` already outlives the pools, so the lock covers the whole lifetime
of every region under the name.

## Process roles

Every ``struct xal`` carries one of three roles, reported by
``xal_get_procrole()``:

``XAL_PROCROLE_SINGLE``
   The default. ``xal_open()`` was called without ``shm_name``, so the pools
   are private anonymous memory and no other process is involved.

``XAL_PROCROLE_PRIMARY``
   ``xal_open()`` was called with ``shm_name`` set and found the name unused.
   This handle owns the shared memory objects: it created them, it is the only
   one allowed to index into them, and it removes them again.

``XAL_PROCROLE_SECONDARY``
   The handle came from ``xal_from_shm()``, or from an ``xal_open()`` that
   found the name already published. It maps the objects read-only and owns
   nothing.

The role decides two things:

**Ownership at close.** ``xal_close()`` always unmaps the pools, but only a
primary (or single) handle also ``shm_unlink()``s the ``_inodes``,
``_extents`` and ``_state`` objects. A secondary detaches without removing
anything, so several secondaries may attach and close independently. The
consequence is that the shared memory objects live as long as the primary
does: once the primary closes, the names are gone and no new secondary can
attach, even though already-attached secondaries keep their mapping valid
until they close.

**Indexing.** ``xal_index()`` returns ``-EINVAL`` on a secondary handle. The
pools are mapped read-only there, and the index is the primary's to build and
rebuild. A secondary that finds the view stale — attaching returns ``-ESTALE``
when the region has been marked dirty — must wait for the primary to re-index
rather than re-index itself.
