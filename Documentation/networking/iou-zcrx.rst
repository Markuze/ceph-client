.. SPDX-License-Identifier: GPL-2.0

=====================
io_uring zero copy Rx
=====================

Introduction
============

io_uring zero copy Rx (ZC Rx) is a feature that removes kernel-to-user copy on
the network receive path, allowing packet data to be received directly into
userspace memory. This feature is different to TCP_ZEROCOPY_RECEIVE in that
there are no strict alignment requirements and no need to mmap()/munmap().
Compared to kernel bypass solutions such as e.g. DPDK, the packet headers are
processed by the kernel TCP stack as normal.

NIC HW Requirements
===================

Several NIC HW features are required for io_uring ZC Rx to work. For now the
kernel API does not configure the NIC and it must be done by the user.
`MAIO mode`_ needs none of them.

Header/data split
-----------------

Required to split packets at the L4 boundary into a header and a payload.
Headers are received into kernel memory as normal and processed by the TCP
stack as normal. Payloads are received into userspace memory directly.

Flow steering
-------------

Specific HW Rx queues are configured for this feature, but modern NICs
typically distribute flows across all HW Rx queues. Flow steering is required
to ensure that only desired flows are directed towards HW queues that are
configured for io_uring ZC Rx.

RSS
---

In addition to flow steering above, RSS is required to steer all other non-zero
copy flows away from queues that are configured for io_uring ZC Rx.

Usage
=====

Setup NIC
---------

Must be done out of band for now.

Ensure there are at least two queues::

  ethtool -L eth0 combined 2

Enable header/data split::

  ethtool -G eth0 tcp-data-split on

Carve out half of the HW Rx queues for zero copy using RSS::

  ethtool -X eth0 equal 1

Set up flow steering, bearing in mind that queues are 0-indexed::

  ethtool -N eth0 flow-type tcp6 ... action 1

Setup io_uring
--------------

This section describes the low level io_uring kernel API. Please refer to
liburing documentation for how to use the higher level API.

Create an io_uring instance with the following required setup flags::

  IORING_SETUP_SINGLE_ISSUER
  IORING_SETUP_DEFER_TASKRUN
  IORING_SETUP_CQE32 or IORING_SETUP_CQE_MIXED

Create memory area
------------------

Allocate userspace memory area for receiving zero copy data::

  void *area_ptr = mmap(NULL, area_size,
                        PROT_READ | PROT_WRITE,
                        MAP_ANONYMOUS | MAP_PRIVATE,
                        0, 0);

Create refill ring
------------------

Allocate memory for a shared ringbuf used for returning consumed buffers::

  void *ring_ptr = mmap(NULL, ring_size,
                        PROT_READ | PROT_WRITE,
                        MAP_ANONYMOUS | MAP_PRIVATE,
                        0, 0);

This refill ring consists of some space for the header, followed by an array of
``struct io_uring_zcrx_rqe``::

  size_t rq_entries = 4096;
  size_t ring_size = rq_entries * sizeof(struct io_uring_zcrx_rqe) + PAGE_SIZE;
  /* align to page size */
  ring_size = (ring_size + (PAGE_SIZE - 1)) & ~(PAGE_SIZE - 1);

Register ZC Rx
--------------

Fill in registration structs::

  struct io_uring_zcrx_area_reg area_reg = {
    .addr = (__u64)(unsigned long)area_ptr,
    .len = area_size,
    .flags = 0,
  };

  struct io_uring_region_desc region_reg = {
    .user_addr = (__u64)(unsigned long)ring_ptr,
    .size = ring_size,
    .flags = IORING_MEM_REGION_TYPE_USER,
  };

  struct io_uring_zcrx_ifq_reg reg = {
    .if_idx = if_nametoindex("eth0"),
    /* this is the HW queue with desired flow steered into it */
    .if_rxq = 1,
    .rq_entries = rq_entries,
    .area_ptr = (__u64)(unsigned long)&area_reg,
    .region_ptr = (__u64)(unsigned long)&region_reg,
  };

Register with kernel::

  io_uring_register_ifq(ring, &reg);

Map refill ring
---------------

The kernel fills in fields for the refill ring in the registration ``struct
io_uring_zcrx_ifq_reg``. Map it into userspace::

  struct io_uring_zcrx_rq refill_ring;

  refill_ring.khead = (unsigned *)((char *)ring_ptr + reg.offsets.head);
  refill_ring.khead = (unsigned *)((char *)ring_ptr + reg.offsets.tail);
  refill_ring.rqes =
    (struct io_uring_zcrx_rqe *)((char *)ring_ptr + reg.offsets.rqes);
  refill_ring.rq_tail = 0;
  refill_ring.ring_ptr = ring_ptr;

Receiving data
--------------

Prepare a zero copy recv request::

  struct io_uring_sqe *sqe;

  sqe = io_uring_get_sqe(ring);
  io_uring_prep_rw(IORING_OP_RECV_ZC, sqe, fd, NULL, 0, 0);
  sqe->ioprio |= IORING_RECV_MULTISHOT;

Now, submit and wait::

  io_uring_submit_and_wait(ring, 1);

Finally, process completions::

  struct io_uring_cqe *cqe;
  unsigned int count = 0;
  unsigned int head;

  io_uring_for_each_cqe(ring, head, cqe) {
    struct io_uring_zcrx_cqe *rcqe = (struct io_uring_zcrx_cqe *)(cqe + 1);

    unsigned long mask = (1ULL << IORING_ZCRX_AREA_SHIFT) - 1;
    unsigned char *data = area_ptr + (rcqe->off & mask);
    /* do something with the data */

    count++;
  }
  io_uring_cq_advance(ring, count);

Recycling buffers
-----------------

Return buffers back to the kernel to be used again::

  struct io_uring_zcrx_rqe *rqe;
  unsigned mask = refill_ring.ring_entries - 1;
  rqe = &refill_ring.rqes[refill_ring.rq_tail & mask];

  unsigned long area_offset = rcqe->off & ~IORING_ZCRX_AREA_MASK;
  rqe->off = area_offset | area_reg.rq_area_token;
  rqe->len = cqe->res;
  IO_URING_WRITE_ONCE(*refill_ring.ktail, ++refill_ring.rq_tail);

Area chunking
-------------

zcrx splits the memory area into fixed-length physically contiguous chunks.
This limits the maximum buffer size returned in a single io_uring CQE. Users
can provide a hint to the kernel to use larger chunks by setting the
``rx_buf_len`` field of ``struct io_uring_zcrx_ifq_reg`` to the desired length
during registration. If this field is set to zero, the kernel defaults to
the system page size.

To use larger sizes, the memory area must be backed by physically contiguous
ranges whose sizes are multiples of ``rx_buf_len``. It also requires kernel
and hardware support. If registration fails, users are generally expected to
fall back to defaults by setting ``rx_buf_len`` to zero.

Larger chunks don't give any additional guarantees about buffer sizes returned
in CQEs, and they can vary depending on many factors like traffic pattern,
hardware offload, etc. It doesn't require any application changes beyond zcrx
registration.

MAIO mode
=========

What MAIO mode is
-----------------

MAIO mode lets a process with ``CAP_NET_ADMIN`` use ``IORING_OP_RECV_ZC`` on a
network device that has no header/data split and no flow steering. The mode
needs no specific NIC. It needs a driver that meets the requirements below.

The kernel allocates a receive area. The process maps the area read-write.
The kernel installs a *provider* of readable pages on every receive queue of
the device. A provider is the memory provider that gives pages to the page
pool of a queue. The queues fill their rings from the area. The stack parses
the headers of a packet as usual. Then the socket decides where the payload
goes. This demultiplexing in the kernel replaces the flow rule of the NIC. A
socket that has a ``RECV_ZC`` request on the area gets a reference to the
payload in the area. Every other socket copies the payload out of the same
pages, as it does without MAIO mode.

MAIO mode is a variant of zcrx. It keeps the registration call, the refill
ring and the format of the CQEs. It also keeps the user references, the copy
fallback, the events, and the export and import of an instance. A zcrx
instance in this mode is a *MAIO instance*. A *slot* is the unit of the area: one page. An *area
page* is the page of a slot.

Requirements compared with hardware zcrx
----------------------------------------

.. list-table::
   :header-rows: 1
   :widths: 20 40 40

   * - Requirement
     - Hardware zcrx
     - MAIO mode
   * - NIC hardware
     - Header/data split and flow steering
     - None
   * - Driver
     - Queue API, ``PP_FLAG_ALLOW_UNREADABLE_NETMEM`` and header/data split
     - Queue API and the header contract, see `Driver contract`_
   * - Privilege
     - ``CAP_NET_ADMIN``
     - ``CAP_NET_ADMIN``
   * - Steering
     - A flow rule for each application, and RSS that excludes the queue
     - None. The socket lookup in the kernel decides.
   * - Payload visible to the process
     - Yes, in memory of the process
     - Yes, in a read-write mapping of kernel memory

Registration
------------

Create the ring and the refill ring as for hardware zcrx. Do not allocate the
area. The kernel does it. Fill in the registration structures::

  struct io_uring_zcrx_area_reg area_reg = {
    .len = area_size,
    .flags = IORING_ZCRX_AREA_KERNEL,
  };

  struct io_uring_zcrx_ifq_reg reg = {
    .if_idx = if_nametoindex("eth0"),
    .if_rxq = 0,
    .rq_entries = rq_entries,
    .flags = ZCRX_REG_MAIO,
    .area_ptr = (__u64)(unsigned long)&area_reg,
    .region_ptr = (__u64)(unsigned long)&region_reg,
  };

The rules are:

* ``ZCRX_REG_MAIO`` and ``IORING_ZCRX_AREA_KERNEL`` need each other. One
  without the other fails with ``EINVAL``.
* ``if_rxq`` is 0. The instance covers all receive queues of the device.
* ``len`` is a non-zero multiple of the page size. The fields ``addr``,
  ``dmabuf_fd``, ``rq_area_token``, ``mmap_offset`` and ``__resv2`` of the
  area are 0.
* ``rx_buf_len`` is 0 or the page size. A slot is one page. The kernel writes
  the page size to ``rx_buf_len``.
* The area counts against the locked memory limit, as a pinned user area
  does. Registration fails with ``ENOMEM`` when the limit is too low.
* The kernel allocates the pages. They are zeroed and not contiguous. The
  kernel writes the offset for ``mmap()`` to ``mmap_offset`` of the area.
* ``ZCRX_REG_NODEV`` can join ``ZCRX_REG_MAIO``. ``if_idx`` is then 0, and
  the kernel copies all data into the area. This combination is for tests.

The process must map the whole area ``MAP_SHARED`` and read-write::

  void *area = mmap(NULL, area_reg.len, PROT_READ | PROT_WRITE, MAP_SHARED,
                    ring_fd, area_reg.mmap_offset);

A ``MAP_PRIVATE`` mapping fails with ``EINVAL``. A mapping that is shorter
than the area fails with ``EFAULT``. A child that calls ``fork()`` does not
inherit the mapping. A core dump does not include it. The mapping cannot
grow. A system without an MMU, and an architecture that needs page colouring
(``SHM_COLOUR``), cannot have the area. Registration fails with
``EOPNOTSUPP`` there.

Registration restarts the receive queues. The kernel takes the lock of the
device and converts each queue in index order through the queue API of the
driver. For each queue, the driver allocates new queue memory with a page
pool that binds the provider. Then it stops the queue and starts it from the
area. Most drivers return the posted buffers to the old pool at the stop.
``virtio_net`` can leave them in the ring, see `Driver contract`_. A queue
pauses during its restart. The link stays up. If one queue fails, the kernel
restores that queue and removes the provider from the queues that it
converted. The device is then as it was, and registration returns the error.

Registration can fail for these reasons, among others:

* ``EPERM``: the task has no ``CAP_NET_ADMIN``.
* ``ENODEV``: the interface does not exist.
* ``EOPNOTSUPP``: the driver has no queue API, or ``virtio_net`` uses the DMA
  API and the device has no ``VIRTIO_F_RING_RESET``.
* ``EEXIST``: a receive queue has a provider already, or an XDP program is
  attached to the device.
* ``EBUSY``: AF_XDP uses a receive queue.
* ``ENETDOWN``: the interface is down, and the driver cannot allocate queue
  memory for a down interface. The device stays as it was. ``virtio_net``
  fails in this way. ``netdevsim`` accepts a down interface, and its pools
  bind the provider when the interface goes up.

While the instance exists, these requests fail:

* A request to set the receive channel count (``ethtool -L``) fails with
  ``EINVAL``, also a request for the current count. The provider must cover
  every queue. Remove the provider first.
* A change of the ring parameters (``ethtool -G``) fails with ``EINVAL``
  unless ``tcp-data-split`` is on and ``hds-thresh`` is 0. This is the check
  of the core for any provider. A device without header/data split cannot
  change its ring parameters while the instance exists.
* ``ZCRX_CTRL_ADD_AREA`` fails with ``EOPNOTSUPP``. A MAIO instance has one
  area.
* A new XDP program or an AF_XDP binding on the device fails.

Unregistration, and the exit of the ring, remove the provider from every
queue. Each queue restarts with an ordinary page pool. A socket buffer, an
object, the process or, on ``virtio_net``, a posted receive buffer can still
hold area pages. The area stays until the last holder drops its page. It
stays mapped for DMA until the last page pool of the instance is gone. When the device goes away first, the core removes the
provider. The instance and its area stay valid until the ring exits.

``ZCRX_CTRL_EXPORT`` and ``ZCRX_REG_IMPORT`` work as for any instance. A ring
that imports a MAIO instance maps the same pages. Its mmap offset depends on
its own ``zcrx_id``. The importing ring can set ``area_ptr`` of
``struct io_uring_zcrx_ifq_reg`` to a ``struct io_uring_zcrx_area_reg``. The
kernel then writes the area size and the mmap offset into it. For an instance
that is not a MAIO instance, ``area_ptr`` must stay 0.

Receiving
---------

Submit ``IORING_OP_RECV_ZC`` as described above. The CQE has the same format.
``res`` is the number of bytes. ``off`` is in the zcrx offset format: the area
id in the bits from ``IORING_ZCRX_AREA_SHIFT`` and the byte offset in the area
below. The bytes are at ``area + (off & ((1ULL << IORING_ZCRX_AREA_SHIFT) - 1))``.
Return a slot through the refill ring as for hardware zcrx. Each CQE holds
one user reference to its slot, so the process returns one refill entry for
each CQE.

The kernel decides for every run of bytes how to pass it:

* **Reference.** The bytes are in one page that a page pool of the instance
  owns. The socket buffer is marked for page pool recycling
  (``skb->pp_recycle``). The CQE names the slot and the offset of the bytes in
  it. The kernel takes a pool reference and then a user reference, so the slot
  cannot return to its pool while the process holds it. Only a marked socket
  buffer owns a pool reference. That reference keeps the pool fields of the
  page stable. An unmarked socket buffer can hold a pool page by a plain
  reference, for example after a splice. The pool may release such a page at
  any time. The kernel copies from it.
* **Copy.** The kernel copies every other readable run of bytes into *copy
  pages*. A copy page is a free slot that no pool owns. The kernel copies at
  most one page of bytes into a copy page and posts one CQE for it. The copy
  starts at offset 0 of the slot.
* **Head.** The kernel copies the linear part of a socket buffer. It passes a
  reference only if the bytes lie in one page of a pool of the instance, and
  the buffer is marked. A driver that meets the header contract puts no
  payload in the head, so this case is rare.
* **Device memory.** A fragment of memory that the CPU cannot read and
  another provider owns ends the request with ``EFAULT``.

The copy cases are: ordinary pages, *fallback pages* (see `Exhaustion`_),
pages of other instances and pools, and every page of an unmarked socket
buffer. An *outsider page* is a page of the area that no pool owns, for
example user memory that the process sent with ``SEND_ZC``. The kernel copies
it too.

The two copy counters of ``struct zcrx_stats`` count copies. A copy takes
the bytes of one fragment, or of the linear part, of one socket buffer, in
whole or in part. ``copy_count`` is the number of copies. It does not count
CQEs. A copy can need several copy pages and post several CQEs. A fragment of
20580 bytes posts 6 CQEs and adds 1 to ``copy_count``. ``copy_bytes`` is the
number of bytes copied. The event ``ZCRX_EVENT_COPY`` fires after a copy. It
does not fire again until the process arms it with ``ZCRX_CTRL_ARM_EVENT``.

Exhaustion
----------

The area can have no free slot. Pools, socket buffers, copies and the process
can hold all of them. The provider serves every queue of the device, so the
host must keep receiving. A hardware zcrx queue stops instead. The provider
gives the page pool *fallback pages*: ordinary pages from the page allocator.
The process cannot see a fallback page. A packet in a fallback page takes the
copy path for a ``RECV_ZC`` request and the ordinary path for every other
socket. ``fallback_pages`` counts these pages.

The first allocation that finds the area empty starts an *episode*. A pool that
needs a page and a request that needs a copy page can both start one. They
share it. The kernel adds 1 to ``exhaust_count`` and sends
``ZCRX_EVENT_ALLOC_FAIL`` if the event is armed. Any allocation that gets a
slot ends the episode. An event fires once. To receive it again, arm it with
``ZCRX_CTRL_ARM_EVENT``. This call fails with ``EINVAL`` if the event did not
fire.

A request that finds the area empty while it copies first passes the bytes
that it copied. Then it ends with a CQE that has the result ``-ENOMEM``. The
bytes that it did not copy stay in the socket. Return slots through the
refill ring and submit a new request to get them.

Four counters in ``struct zcrx_stats`` follow ``copy_count`` and
``copy_bytes``. They count for a MAIO instance:

* ``limbo_enter``: pages that a pool released while an outsider held a
  reference. See `Page states`_.
* ``limbo_leave``: parked pages that became free again.
* ``fallback_pages``: ordinary pages that the provider gave to a pool.
* ``exhaust_count``: episodes.

The structure grows only at the end. ``IO_URING_QUERY_ZCRX_EVENT`` reports
the size that the kernel uses. It is 48 bytes in this version. Registration
needs that much room at ``stats_offset`` in the refill ring region.

Page states
-----------

A slot is free, in a pool, held, or in limbo. A *free* slot is in the freelist
of the area. A slot *in a pool* belongs to the page pool of a queue. It is in
the cache or the ring of the pool, in a descriptor of the device, or in a
socket buffer. A *held* slot is a slot in a pool that has extra references:
the user references of ``RECV_ZC`` CQEs, or the extents of opaque objects. A
page *in limbo* was released by its pool while an outsider still held a
reference. An *outsider* is any holder of a page reference other than the
area, the pool and the PTEs of the mapping. A transmit queue after a send, a
splice and a pinned user buffer are outsiders. A page waits in limbo until
the outsider drops its reference. Then it becomes free. The provider never
gives a page to a device while an outsider holds it. A copy page has no pool.
It returns to the freelist, or to limbo, when its last holder drops it. A page
never reaches the page allocator while the area exists.

Driver contract
---------------

The process can write every area page. The kernel must not parse a header
from memory that a process can write. A driver that uses MAIO mode keeps every
header that the stack parses in a private head of the socket buffer. It
attaches only payload to the socket buffer, as fragments of the pool page. It
must not build the socket buffer over a pool page, as ``build_skb()`` does.

The driver tells the page pool that its pages meet this rule. It sets
``PP_FLAG_PRIVATE_HEADERS`` in ``struct page_pool_params``. It also sets
``netdev`` and ``queue_idx``, so that the pool finds the provider of its
queue. A pool that sets ``PP_FLAG_ALLOW_UNREADABLE_NETMEM`` binds the provider
too. The flag ``PP_MP_F_READABLE`` marks a provider of readable pages. It
tells the core that the queue needs no header/data split and the pool needs no
DMA mapping. A pool that maps DMA uses ``DMA_FROM_DEVICE``, and all queues use
one DMA device. The pool has order 0. The driver implements the queue API. It
creates the new page pool of a queue in ``ndo_queue_mem_alloc()``.

Drivers differ in how they meet the contract:

* A driver that sets ``PP_FLAG_ALLOW_UNREADABLE_NETMEM`` already keeps headers
  out of that pool. It needs no change. The drivers are bnxt, mlx5, gve,
  fbnic and bnge at this version. This series did not test them.
* ``virtio_net`` meets the contract with the queue API and the copy of
  headers that this series adds. A receive queue that has a provider of
  readable pages marks its pool with ``PP_FLAG_PRIVATE_HEADERS``. For every
  page of such a pool, also one that the queue posted before it got another
  pool, ``page_to_skb()`` allocates a head of ``GOOD_COPY_LEN`` (128) bytes. It
  copies the headers that ``eth_get_headlen()`` finds in the first 128 bytes
  of the frame, and nothing else, so the payload stays in the page. If the
  dissector finds no header beyond the Ethernet header, it copies the frame,
  up to 128 bytes. Headers longer than 128 bytes stay partly in the page. The
  stack pulls the rest into the head with ``pskb_may_pull()``. This is a
  copy. The stack never parses a header in place in the page. The driver
  reads the virtio-net header of such a page once, into a private copy, and
  uses only the copy. It drops a buffer whose copy has ``num_buffers`` 0 or
  larger than the ring.
* ``virtio_net`` replaces the pool of a queue without a reset of the
  virtqueue if the device does not use the DMA API. The buffers of the old
  pool stay in the ring and return to that pool when the stack frees them.
  The new pool fills the slots that the device frees, so a few hundred
  packets per queue after the restart take the copy path. This mode needs no
  ``VIRTIO_F_RING_RESET``, and it does not depend on how a host handles a
  queue reset. A device that uses the DMA API (``VIRTIO_F_ACCESS_PLATFORM``)
  resets the virtqueue at the stop, because ``page_pool_destroy()`` unmaps
  every page of a pool that maps DMA, also a posted one. Such a device needs
  ``VIRTIO_F_RING_RESET``, and registration fails with ``EOPNOTSUPP``
  without it.
* The pools of a ``virtio_net`` queue live while the device exists.
  ``virtnet_open()`` replaces a pool whose provider came or went while the
  interface was down. While the interface is down, ``ndo_queue_mem_alloc()``
  returns ``-ENETDOWN``.
* ``netdevsim`` has a test aid. If the pool of a queue has a provider of
  readable pages, the receive path moves the payload of each frame into pool
  pages. It keeps the headers in the head. The tests use this aid. It is not
  a model of a real driver.

Trust model
-----------

A process with ``CAP_NET_ADMIN`` can already see all traffic of the host with
``AF_PACKET``. The area gives no new view. The process can also write the
pages. A write changes the bytes that every holder of the page sees. Other
socket buffers and objects can share the page. A page can hold the bytes of
more than one flow, for example when ``virtio_net`` puts several receive
buffers in one page. A send that references the page transmits the bytes that
are present when TCP builds the segment. This is the integrity power of
packet injection, which ``CAP_NET_ADMIN`` and ``CAP_NET_RAW`` grant. The
process can map nothing except the area. The kernel never parses a header from
an area page, so a write cannot change how the kernel parses a packet.

The kernel checks the privilege at registration. An import keeps the privilege
decision of the exporter. An unprivileged process cannot use the mode. A guest
cannot use it across the VM boundary.

Limits
------

* One area for each instance. A slot is one page, so ``rx_buf_len`` is the page
  size.
* No DMA-buf area. No huge page mapping. No placement of pages on a NUMA
  node: the page allocator chooses.
* The kernel allocates the whole area at registration.
* An instance covers the queues that the device has at registration. The
  kernel refuses a request to set the channel count while the instance exists.
* Another process can map an area page, for example with
  ``TCP_ZEROCOPY_RECEIVE``. The provider counts such a PTE as a PTE of the
  mapping of the area. It can reuse the page while the other process still
  maps it.
* The ``netdevsim`` aid is for tests.

Testing
=======

See ``tools/testing/selftests/drivers/net/hw/iou-zcrx.c``

MAIO mode needs no network hardware for its tests.
``tools/testing/selftests/io_uring/zcrx_maio.c`` tests the registration, the
mapping, the limits, the receive paths, exhaustion, and the installation on
``netdevsim`` ports. The KUnit suite ``io_uring-zcrx-maio``
(``CONFIG_IO_URING_ZCRX_KUNIT_TEST``) tests the page states and the lookup of
a page in the area.
