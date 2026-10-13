# OPAQUE_OBJ v9, round 2: memory-policy decisions (implementation guide)

**Base:** local branch `opaque-obj-v9-round1` at `19e616d733c8`: five patches on
`af32da41b032`. Every old block below is quoted from that commit.

**Goal.** Implement the owner's four memory-policy decisions, nothing else:

1. **L: watermarks measure payload held.** In-flight compaction copies and reply framing
   no longer count toward `high_watermark`/`low_watermark`, so a compaction that lowers
   occupancy can no longer switch the store into LOWMEM.
2. **R: `reply_reserve` is a floor.** Framing uses ordinary capture capacity first and the
   reserve only when that is full, so replies blocked on slow peers cannot starve a
   healthy reply while the store is mostly empty.
3. **Same-offset INSPECT is documented.** Behaviour is unchanged; add one sentence to the
   INSPECT section and a selftest that pins it.
4. **Forwarding-only is advisory.** Lowmem admission still marks objects
   `IORING_OPAQUE_OBJECT_F_FORWARD_ONLY`, but ordinary SEND, fan-out and retry are allowed.

This document is the complete, normative specification. Follow it literally:

- **Every old block appears exactly once** in the base. Replace it with the new block
  exactly, including comments and blank lines.
- **Where it says "do not change", leave that code alone,** even if it looks related.
- **If something seems wrong or impossible, stop and report it** instead of improvising.

**How this guide was checked.** I applied every change below to a private prototype of
`19e616d733c8`, autosquashed into five patches with the exact messages of §6, and ran it
under UML with 2 vCPUs, KASAN, lockdep and PREEMPT:

- **Full suite:** KUnit 28/28 and selftests 97/97 with no skips, including all 10,000 SMP
  SEND races, and no kernel diagnostics.
- **Fails before, passes after:** each changed or new test, run alone against the round-1
  kernel, bails out at the assertion named in §8.6, and passes on the prototype.
  `test_reinspect_same_offset()` passes on both; it pins existing behaviour.
- **Stress:** the four changed tests and `test_reply_reserve()` passed 24 repeated runs.
- **checkpatch:** `checkpatch.pl --strict` reports only the missing `Signed-off-by` and the
  MAINTAINERS file-path warnings; every commit message line is at most 75 columns.
- **Builds:** the build has no warnings and `W=1` is clean on `io_uring/`.

The prototype isn't part of this document; implement from the text.

---

## 1. Design, so you understand the change

**L.** `store->bytes` keeps charging everything, so the hard limit is unchanged.

- A new `store->transient` counts bytes that are charged but are not payload held:
  - unpublished compaction copies, from the reservation until the compaction request
    ends;
  - framing charged to ordinary capacity.
- `io_opaque_memory_mode()` compares `bytes - transient` with the watermarks.
- A `seqcount_spinlock_t` makes every update that moves `bytes` and `transient` together a
  single write, so the watermark check never sees one without the other.
  - Plain atomics are not enough: a reader can then see an uncharge of `bytes` without
    the matching `transient` change, or the reverse, and overestimate.
  - Writers take `store->transient_lock`, a leaf lock that never calls back into
    accounting.
- A compaction request ends in `io_opaque_compact_end()`. It runs after the request has
  dropped its references to the old version and to any unpublished replacement. It
  releases the leftover reservation and the exclusion in one write.

**R.** `io_opaque_charge_frame()` first tries ordinary capacity: the capture limit, through
`io_opaque_charge_transient()`. If that is full, it charges the separate reserve counter,
`store->reserve_framing`, which payload capture and compaction still cannot borrow.

- A request records which pool it charged in `op->frame_reserved`, and cleanup releases
  that pool.
- `store->framing` still reports total framing for STAT; `backing_bytes` adds only the
  reserve part, because ordinary framing is already in `bytes`.
- Compaction keeps its `hard_limit - reply_reserve` limit.

**Forwarding-only.** Only the `-EOPNOTSUPP` check in `io_opaque_send_acquire()` is
removed. Classification at admission, the completion and OBJECT_STAT flag, preservation
through compaction, and exclusion from automatic compaction all stay.

---

## 2. Kernel, patch 2/5

### 2.1 `io_uring/opaque_internal.h`: store fields

Old (`io_uring/opaque_internal.h`):

```c
	atomic64_t framing;
	atomic64_t framing_copied;
	atomic_t extents;
```

New:

```c
	atomic64_t framing;
	atomic64_t framing_copied;
	/* Framing held in the reply reserve, outside bytes. */
	atomic64_t reserve_framing;
	/*
	 * Charged in bytes but not payload held: unpublished compaction copies
	 * and framing in ordinary capacity. Watermarks subtract it. Changes
	 * that move bytes and transient together are one seqcount write.
	 */
	atomic64_t transient;
	/* Serializes transient_seq writers. */
	spinlock_t transient_lock;
	seqcount_spinlock_t transient_seq;
	atomic_t extents;
```

### 2.2 `io_uring/opaque_internal.h`: declarations

Old (`io_uring/opaque_internal.h`):

```c
bool io_opaque_charge_frame(struct io_opaque_store *store, u64 bytes);
void io_opaque_uncharge_frame(struct io_opaque_store *store, u64 bytes);
```

New:

```c
bool io_opaque_charge_transient(struct io_opaque_store *store, u64 bytes, u64 limit);
void io_opaque_release_transient(struct io_opaque_store *store, u64 uncharge,
				 u64 transient);
bool io_opaque_charge_frame(struct io_opaque_store *store, u64 bytes, bool *reserved);
void io_opaque_uncharge_frame(struct io_opaque_store *store, u64 bytes, bool reserved);
```

Old (`io_uring/opaque_internal.h`):

```c
int io_opaque_compact_publish(struct io_opaque_req *op);
```

New:

```c
int io_opaque_compact_publish(struct io_opaque_req *op);
void io_opaque_compact_end(struct io_opaque_req *op);
```

### 2.3 `io_uring/opaque.h`: request fields

Old (`io_uring/opaque.h`):

```c
			u16 send_flags;
			bool tx_started;
		};
```

New:

```c
			u16 send_flags;
			bool tx_started;
			bool frame_reserved;
		};
```

Old (`io_uring/opaque.h`):

```c
			struct io_opaque_data *replacement;
			u64 reserved;
		};
```

New:

```c
			struct io_opaque_data *replacement;
			u64 reserved;
			u64 transient;
		};
```

### 2.4 `io_uring/opaque.c`: `io_opaque_memory_mode()`

Old (`io_uring/opaque.c`):

```c
	int mode, next;
	u64 used;

	if (!store->config.high_watermark)
		return IORING_OPAQUE_MEMORY_NORMAL;
	for (;;) {
		mode = atomic_read(&store->memory_mode);
		used = atomic64_read(&store->bytes);
		next = mode;
```

New:

```c
	unsigned int seq;
	int mode, next;
	u64 used, transient;

	if (!store->config.high_watermark)
		return IORING_OPAQUE_MEMORY_NORMAL;
	for (;;) {
		mode = atomic_read(&store->memory_mode);
		/* Watermarks measure payload held: compaction copies and framing are not. */
		do {
			seq = read_seqcount_begin(&store->transient_seq);
			used = atomic64_read(&store->bytes);
			transient = atomic64_read(&store->transient);
		} while (read_seqcount_retry(&store->transient_seq, seq));
		used = used > transient ? used - transient : 0;
		next = mode;
```

### 2.5 `io_uring/opaque.c`: transient helpers and floor framing

Replace the two framing functions with these four.

Old (`io_uring/opaque.c`):

```c
bool io_opaque_charge_frame(struct io_opaque_store *store, u64 bytes)
{
	u64 limit = store->config.reply_reserve;
	s64 used = atomic64_read(&store->framing);

	if (!limit) {
		if (!io_opaque_charge(store, bytes, true))
			return false;
		atomic64_add(bytes, &store->framing);
		return true;
	}
	for (;;) {
		if (bytes > limit || used > limit - bytes)
			return false;
		if (atomic64_try_cmpxchg(&store->framing, &used, used + bytes))
			return true;
	}
}

void io_opaque_uncharge_frame(struct io_opaque_store *store, u64 bytes)
{
	atomic64_sub(bytes, &store->framing);
	if (!store->config.reply_reserve)
		io_opaque_uncharge(store, bytes);
}
```

New:

```c
/* Charge bytes that watermarks ignore; bytes and transient change in one write. */
bool io_opaque_charge_transient(struct io_opaque_store *store, u64 bytes, u64 limit)
{
	s64 used;
	bool charged = false;

	spin_lock(&store->transient_lock);
	write_seqcount_begin(&store->transient_seq);
	used = atomic64_read(&store->bytes);
	for (;;) {
		if (bytes > limit || used > limit - bytes)
			break;
		if (atomic64_try_cmpxchg(&store->bytes, &used, used + bytes)) {
			atomic64_add(bytes, &store->transient);
			charged = true;
			break;
		}
	}
	write_seqcount_end(&store->transient_seq);
	spin_unlock(&store->transient_lock);
	return charged;
}

/* Uncharge and stop excluding transient bytes; the two amounts may differ. */
void io_opaque_release_transient(struct io_opaque_store *store, u64 uncharge,
				 u64 transient)
{
	spin_lock(&store->transient_lock);
	write_seqcount_begin(&store->transient_seq);
	atomic64_sub(uncharge, &store->bytes);
	atomic64_sub(transient, &store->transient);
	write_seqcount_end(&store->transient_seq);
	spin_unlock(&store->transient_lock);
	io_opaque_memory_mode(store);
	if (uncharge)
		io_opaque_wake_budget(store);
}

/* Framing uses ordinary capacity first and the reply reserve only when that is full. */
bool io_opaque_charge_frame(struct io_opaque_store *store, u64 bytes, bool *reserved)
{
	u64 limit = store->config.reply_reserve;
	s64 used;

	if (io_opaque_charge_transient(store, bytes, io_opaque_rx_limit(store))) {
		*reserved = false;
	} else {
		used = atomic64_read(&store->reserve_framing);
		for (;;) {
			if (bytes > limit || used > limit - bytes)
				return false;
			if (atomic64_try_cmpxchg(&store->reserve_framing, &used, used + bytes))
				break;
		}
		*reserved = true;
	}
	atomic64_add(bytes, &store->framing);
	return true;
}

void io_opaque_uncharge_frame(struct io_opaque_store *store, u64 bytes, bool reserved)
{
	atomic64_sub(bytes, &store->framing);
	if (reserved)
		atomic64_sub(bytes, &store->reserve_framing);
	else
		io_opaque_release_transient(store, bytes, bytes);
}
```

### 2.6 `io_uring/opaque.c`: store initialization in `io_opaque_alloc()`

Old (`io_uring/opaque.c`):

```c
	spin_lock_init(&store->wait_lock);
	INIT_LIST_HEAD(&store->budget_waits);
```

New:

```c
	spin_lock_init(&store->wait_lock);
	spin_lock_init(&store->transient_lock);
	seqcount_spinlock_init(&store->transient_seq, &store->transient_lock);
	INIT_LIST_HEAD(&store->budget_waits);
```

### 2.7 `io_uring/opaque.c`: STAT in `io_opaque_stat()`

Old (`io_uring/opaque.c`):

```c
	if (store->config.reply_reserve)
		stat.backing_bytes += stat.framing_bytes;
```

New:

```c
	/* Framing in the reply reserve is charged outside bytes. */
	stat.backing_bytes += atomic64_read(&store->reserve_framing);
```

### 2.8 `io_uring/opaque.c`: request cleanup

Old (`io_uring/opaque.c`):

```c
		if (op->frame_charge)
			io_opaque_uncharge_frame(store, op->frame_charge);
```

New:

```c
		if (op->frame_charge)
			io_opaque_uncharge_frame(store, op->frame_charge, op->frame_reserved);
```

Old (`io_uring/opaque.c`):

```c
	if (op->op == IORING_OPAQUE_COMPACT) {
		io_opaque_data_put(store, op->replacement);
		if (op->reserved)
			io_opaque_uncharge(store, op->reserved);
	}
```

New:

```c
	if (op->op == IORING_OPAQUE_COMPACT) {
		io_opaque_data_put(store, op->replacement);
		io_opaque_compact_end(op);
	}
```

### 2.9 `io_uring/opaque.c`: send framing charge in send preparation

Old (`io_uring/opaque.c`):

```c
			if (!io_opaque_charge_frame(op->store, charge))
				return -ENOBUFS;
```

New:

```c
			if (!io_opaque_charge_frame(op->store, charge, &op->frame_reserved))
				return -ENOBUFS;
```

### 2.10 `io_uring/opaque.c`: `io_opaque_send_acquire()`; forwarding-only is advisory

Old (`io_uring/opaque.c`):

```c
	if (!slot || !slot->data)
		return -ESTALE;
	if (slot->data->forward_only && !(op->send_flags & IORING_OPAQUE_SEND_LAST))
		return -EOPNOTSUPP;
	if (op->offset > slot->data->length ||
```

New:

```c
	if (!slot || !slot->data)
		return -ESTALE;
	if (op->offset > slot->data->length ||
```

### 2.11 `io_uring/opaque_compact.c`

The reservation in `io_opaque_compact_build()`:

Old (`io_uring/opaque_compact.c`):

```c
	/* Allocated replacement bytes remain inside this reservation. */
	if (!io_opaque_charge(store, charge, false))
		return -ENOBUFS;
	op->reserved = charge;
```

New:

```c
	/* Allocated replacement bytes remain inside this reservation. */
	if (!io_opaque_charge_transient(store, charge,
					store->config.hard_limit - store->config.reply_reserve))
		return -ENOBUFS;
	/* Watermarks ignore the copy until io_opaque_compact_end(). */
	op->transient = charge;
	op->reserved = charge;
```

The new end function, placed before `io_opaque_compact_publish()`:

Old (`io_uring/opaque_compact.c`):

```c
int io_opaque_compact_publish(struct io_opaque_req *op)
{
```

New:

```c
/* The copy is published or released; what remains charged is payload held. */
void io_opaque_compact_end(struct io_opaque_req *op)
{
	if (!op->transient)
		return;
	io_opaque_release_transient(op->store, op->reserved, op->transient);
	op->reserved = 0;
	op->transient = 0;
}

int io_opaque_compact_publish(struct io_opaque_req *op)
{
```

Automatic compaction in `io_opaque_auto_compact()`:

Old (`io_uring/opaque_compact.c`):

```c
		io_opaque_data_put(store, op.data);
		io_opaque_data_put(store, op.replacement);
		if (op.reserved)
			io_opaque_uncharge(store, op.reserved);
```

New:

```c
		io_opaque_data_put(store, op.data);
		io_opaque_data_put(store, op.replacement);
		io_opaque_compact_end(&op);
```

That is the whole kernel change. Do not change:

- the UAPI header;
- `io_opaque_charge()`, `io_opaque_uncharge()`, `io_opaque_wake_budget()` or the RX limits;
- forwarding-only classification at admission, its completion and OBJECT_STAT flags,
  `replacement->forward_only = op->data->forward_only`, or the two places that keep
  forwarding-only objects out of automatic compaction;
- `io_opaque_compact_publish()` and `io_opaque_compact_start()`.

---

## 3. KUnit, patch 3/5: `io_uring/opaque_test.c`

The suite goes from 27 to 28 cases.

### 3.1 Initialize the new lock in `opaque_test_store()`

Old (`io_uring/opaque_test.c`):

```c
	mutex_init(&store->tables);
	spin_lock_init(&store->wait_lock);
```

New:

```c
	mutex_init(&store->tables);
	spin_lock_init(&store->wait_lock);
	spin_lock_init(&store->transient_lock);
	seqcount_spinlock_init(&store->transient_seq, &store->transient_lock);
```

### 3.2 `opaque_compact_versions()`: end the compaction

Old (`io_uring/opaque_test.c`):

```c
	io_opaque_data_put(store, data); /* worker */
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes),
			(s64)(40 * PAGE_SIZE + ALIGN(80000, PAGE_SIZE)));
	io_opaque_data_put(store, data); /* old read/send */
```

New:

```c
	io_opaque_data_put(store, data); /* worker */
	io_opaque_compact_end(&op);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->transient), 0LL);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes),
			(s64)(40 * PAGE_SIZE + ALIGN(80000, PAGE_SIZE)));
	io_opaque_data_put(store, data); /* old read/send */
```

### 3.3 New case `opaque_compact_watermarks()`, placed before `opaque_compact_reservation()`

Old (`io_uring/opaque_test.c`):

```c
static void opaque_compact_reservation(struct kunit *test)
```

New:

```c
static void opaque_compact_watermarks(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	struct io_opaque_data *data = opaque_sparse_data(test, store);
	struct io_opaque_req op = {
		.store = store, .data = data, .handle = 9ULL << IO_OPAQUE_INDEX_BITS,
	};
	u64 sparse = 40 * PAGE_SIZE, dense = ALIGN(80000, PAGE_SIZE);

	/* Counting the copy would cross the high watermark. */
	store->config.high_watermark = sparse + PAGE_SIZE;
	store->config.low_watermark = PAGE_SIZE;
	store->objects[0].data = data;
	store->objects[0].generation = 9;
	store->nr_objects++;
	__set_bit(0, store->object_used);
	refcount_inc(&data->refs); /* worker */
	KUNIT_EXPECT_EQ(test, io_opaque_memory_mode(store), IORING_OPAQUE_MEMORY_NORMAL);
	KUNIT_ASSERT_EQ(test, io_opaque_compact_build(&op), 0);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), (s64)(sparse + dense));
	KUNIT_EXPECT_EQ(test, io_opaque_memory_mode(store), IORING_OPAQUE_MEMORY_NORMAL);
	KUNIT_ASSERT_EQ(test, io_opaque_compact_publish(&op), 0);
	io_opaque_data_put(store, op.data); /* worker */
	io_opaque_compact_end(&op);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->transient), 0LL);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), (s64)dense);
	KUNIT_EXPECT_EQ(test, io_opaque_memory_mode(store), IORING_OPAQUE_MEMORY_NORMAL);
	io_opaque_data_put(store, store->objects[0].data);
	store->objects[0].data = NULL;
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
}

static void opaque_compact_reservation(struct kunit *test)
```

### 3.4 `opaque_reply_reserve()` for floor semantics (whole function)

Old (`io_uring/opaque_test.c`):

```c
static void opaque_reply_reserve(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);

	store->config.hard_limit = 8 * PAGE_SIZE;
	store->config.compact_headroom = 2 * PAGE_SIZE;
	store->config.reply_reserve = 2 * PAGE_SIZE;
	KUNIT_ASSERT_TRUE(test, io_opaque_charge(store, 4 * PAGE_SIZE, true));
	KUNIT_EXPECT_FALSE(test, io_opaque_charge(store, PAGE_SIZE, true));
	KUNIT_ASSERT_TRUE(test, io_opaque_charge_frame(store, 2 * PAGE_SIZE));
	KUNIT_EXPECT_FALSE(test, io_opaque_charge_frame(store, 1));
	KUNIT_ASSERT_TRUE(test, io_opaque_charge(store, 2 * PAGE_SIZE, false));
	KUNIT_EXPECT_FALSE(test, io_opaque_charge(store, PAGE_SIZE, false));
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes) + atomic64_read(&store->framing),
			(s64)store->config.hard_limit);
	io_opaque_uncharge_frame(store, 2 * PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), (s64)6 * PAGE_SIZE);
	io_opaque_uncharge(store, 6 * PAGE_SIZE);
	store->config.reply_reserve = 0;
	KUNIT_ASSERT_TRUE(test, io_opaque_charge(store, 6 * PAGE_SIZE, true));
	KUNIT_EXPECT_FALSE(test, io_opaque_charge_frame(store, 1));
	io_opaque_uncharge(store, 6 * PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->framing), 0LL);
}
```

New:

```c
static void opaque_reply_reserve(struct kunit *test)
{
	struct io_opaque_store *store = opaque_test_store(test);
	bool reserved;

	store->config.hard_limit = 8 * PAGE_SIZE;
	store->config.compact_headroom = 2 * PAGE_SIZE;
	store->config.reply_reserve = 2 * PAGE_SIZE;
	/* Framing uses ordinary capacity first. */
	KUNIT_ASSERT_TRUE(test, io_opaque_charge_frame(store, PAGE_SIZE, &reserved));
	KUNIT_EXPECT_FALSE(test, reserved);
	KUNIT_ASSERT_TRUE(test, io_opaque_charge(store, 3 * PAGE_SIZE, true));
	KUNIT_EXPECT_FALSE(test, io_opaque_charge(store, 1, true));
	/* With ordinary capacity full, only framing may use the reserve. */
	KUNIT_ASSERT_TRUE(test, io_opaque_charge_frame(store, 2 * PAGE_SIZE, &reserved));
	KUNIT_EXPECT_TRUE(test, reserved);
	KUNIT_EXPECT_FALSE(test, io_opaque_charge_frame(store, 1, &reserved));
	/* Compaction may use its headroom, never the reserve. */
	KUNIT_ASSERT_TRUE(test, io_opaque_charge(store, 2 * PAGE_SIZE, false));
	KUNIT_EXPECT_FALSE(test, io_opaque_charge(store, 1, false));
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes) +
			atomic64_read(&store->reserve_framing), (s64)store->config.hard_limit);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->framing), (s64)3 * PAGE_SIZE);
	io_opaque_uncharge_frame(store, 2 * PAGE_SIZE, true);
	io_opaque_uncharge_frame(store, PAGE_SIZE, false);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), (s64)5 * PAGE_SIZE);
	io_opaque_uncharge(store, 5 * PAGE_SIZE);
	store->config.reply_reserve = 0;
	KUNIT_ASSERT_TRUE(test, io_opaque_charge(store, 6 * PAGE_SIZE, true));
	KUNIT_EXPECT_FALSE(test, io_opaque_charge_frame(store, 1, &reserved));
	io_opaque_uncharge(store, 6 * PAGE_SIZE);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->bytes), 0LL);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->framing), 0LL);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->reserve_framing), 0LL);
	KUNIT_EXPECT_EQ(test, atomic64_read(&store->transient), 0LL);
}
```

### 3.5 `opaque_forward_only_versions()`: advisory send admission

Old (`io_uring/opaque_test.c`):

```c
	struct io_opaque_req send = {
		.store = store, .handle = compact.handle, .length = 80000,
	};

	data->forward_only = true;
	store->objects[0].data = data;
	store->objects[0].generation = 9;
	store->nr_objects++;
	__set_bit(0, store->object_used);
	refcount_inc(&data->refs);
	KUNIT_ASSERT_EQ(test, io_opaque_compact_build(&compact), 0);
	KUNIT_EXPECT_TRUE(test, compact.replacement->forward_only);
	KUNIT_ASSERT_EQ(test, io_opaque_compact_publish(&compact), 0);
	mutex_lock(&store->tables);
	KUNIT_EXPECT_EQ(test, io_opaque_send_acquire(&send), -EOPNOTSUPP);
	KUNIT_EXPECT_FALSE(test, send.consumed);
	send.send_flags = IORING_OPAQUE_SEND_LAST;
	KUNIT_EXPECT_EQ(test, io_opaque_send_acquire(&send), 0);
	mutex_unlock(&store->tables);
	KUNIT_EXPECT_TRUE(test, send.consumed);
	KUNIT_EXPECT_TRUE(test, send.data->forward_only);
	io_opaque_data_put(store, compact.data);
	io_opaque_data_put(store, send.data);
```

New:

```c
	struct io_opaque_req send = {
		.store = store, .handle = compact.handle, .length = 80000,
	};
	struct io_opaque_req last = {
		.store = store, .handle = compact.handle, .length = 80000,
		.send_flags = IORING_OPAQUE_SEND_LAST,
	};

	data->forward_only = true;
	store->objects[0].data = data;
	store->objects[0].generation = 9;
	store->nr_objects++;
	__set_bit(0, store->object_used);
	refcount_inc(&data->refs);
	KUNIT_ASSERT_EQ(test, io_opaque_compact_build(&compact), 0);
	KUNIT_EXPECT_TRUE(test, compact.replacement->forward_only);
	KUNIT_ASSERT_EQ(test, io_opaque_compact_publish(&compact), 0);
	mutex_lock(&store->tables);
	/* The classification is advisory: an ordinary send pins a version. */
	KUNIT_EXPECT_EQ(test, io_opaque_send_acquire(&send), 0);
	KUNIT_EXPECT_FALSE(test, send.consumed);
	KUNIT_EXPECT_EQ(test, io_opaque_send_acquire(&last), 0);
	mutex_unlock(&store->tables);
	KUNIT_EXPECT_TRUE(test, last.consumed);
	KUNIT_EXPECT_TRUE(test, send.data->forward_only);
	KUNIT_EXPECT_PTR_EQ(test, send.data, last.data);
	io_opaque_data_put(store, compact.data);
	io_opaque_compact_end(&compact);
	io_opaque_data_put(store, send.data);
	io_opaque_data_put(store, last.data);
```

### 3.6 Case list

Old (`io_uring/opaque_test.c`):

```c
	KUNIT_CASE(opaque_compact_versions),
	KUNIT_CASE(opaque_compact_reservation),
```

New:

```c
	KUNIT_CASE(opaque_compact_versions),
	KUNIT_CASE(opaque_compact_watermarks),
	KUNIT_CASE(opaque_compact_reservation),
```

---

## 4. Selftests, patch 4/5: `tools/testing/selftests/io_uring/opaque_obj.c`

The plan goes from 94 to 97 groups.

### 4.1 `test_lowmem()`: advisory forwarding-only sends

Old (`tools/testing/selftests/io_uring/opaque_obj.c`):

```c
	require(event.extra[1] == IORING_OPAQUE_OBJECT_F_FORWARD_ONLY,
		"lowmem object-ready reports forwarding-only ownership");
```

New:

```c
	require(event.extra[1] == IORING_OPAQUE_OBJECT_F_FORWARD_ONLY,
		"lowmem object-ready reports forwarding-only classification");
```

Old (`tools/testing/selftests/io_uring/opaque_obj.c`):

```c
	tag = send_stage(&imported, other_context, target[0], forward, 0, length);
	require(wait_tag_ms(&imported, tag, 5000).res == -EOPNOTSUPP,
		"lowmem object forbids reusable SEND");
```

New:

```c
	tag = send_stage(&imported, other_context, target[0], forward, 0, length);
	require(wait_tag_ms(&imported, tag, 5000).res == (int)length,
		"forwarding-only classification is advisory for SEND");
	read_all(target[1], wire, length);
	require(!memcmp(wire, data, length), "advisory SEND wire bytes");
```

Old (`tools/testing/selftests/io_uring/opaque_obj.c`):

```c
	tag = send_stage(&r, context, target[0], forward, 0, length);
	require(wait_tag_ms(&r, tag, 5000).res == -EOPNOTSUPP,
		"forwarding restriction survives normal mode and compaction");
```

New:

```c
	require(!command(&r, context, IORING_OPAQUE_OBJECT_STAT, forward, 0,
			 sizeof(object), &object).res &&
		(object.flags & IORING_OPAQUE_OBJECT_F_FORWARD_ONLY),
		"classification survives normal mode and compaction");
	tag = send_stage(&r, context, target[0], forward, 0, length);
	require(wait_tag_ms(&r, tag, 5000).res == (int)length, "advisory fan-out SEND");
	read_all(target[1], wire, length);
```

Old (`tools/testing/selftests/io_uring/opaque_obj.c`):

```c
	ksft_test_result_pass("forwarding-only ownership survives mode changes and compaction\n");
```

New:

```c
	ksft_test_result_pass("forwarding-only flag survives mode changes and compaction\n");
```

### 4.2 Three new tests, inserted after `test_gap_after_decision()` and before `drain_idle()`

They use only existing helpers. Their order in the file is
`test_reinspect_same_offset()`, `test_compact_watermarks()` and
`test_reply_reserve_floor()`.

Old (`tools/testing/selftests/io_uring/opaque_obj.c`):

```c
	ksft_test_result_pass("a decision past an uncollected gap completes in a held pass\n");
}

static size_t drain_idle(int fd, unsigned char *buf, size_t cap, int idle_ms)
```

New:

```c
	ksft_test_result_pass("a decision past an uncollected gap completes in a held pass\n");
}

/* Re-inspecting an offset of a held stream returns only bytes already collected. */
static void test_reinspect_same_offset(struct io_uring_opaque_config cfg)
{
	unsigned char part[100], buf[512];
	struct ring r;
	uint32_t context;
	uint64_t stream, tag;
	int pair[2];

	memset(part, 'h', sizeof(part));
	require(!ring_init(&r, IORING_SETUP_CQE32) && !register_store(&r, &cfg, &context),
		"same-offset inspect store");
	tcp_pair(pair);
	stream = attach(&r, context, pair[1], next_tag++);
	write_all(pair[0], part, sizeof(part));
	settle(&r);
	require(command(&r, context, IORING_OPAQUE_INSPECT, stream, 0, sizeof(buf),
			buf).res == sizeof(part), "inspect the first part");
	write_all(pair[0], part, sizeof(part));
	settle(&r);
	require(command(&r, context, IORING_OPAQUE_INSPECT, stream, 0, sizeof(buf),
			buf).res == sizeof(part) && unread_bytes(pair[1]) == sizeof(part),
		"same-offset INSPECT leaves held bytes on TCP");
	tag = cmd_stage(&r, context, IORING_OPAQUE_INSPECT, stream, sizeof(part),
			sizeof(buf), buf);
	require(wait_tag_ms(&r, tag, 5000).res == sizeof(part),
		"end-offset INSPECT collects the held bytes");
	require(!command(&r, context, IORING_OPAQUE_STREAM_CLOSE, stream, 0, 0, NULL).res,
		"same-offset inspect close");
	ring_exit(&r);
	close(pair[0]);
	close(pair[1]);
	ksft_test_result_pass("re-inspecting an offset returns only collected bytes\n");
}

/* Compaction copies do not count toward the watermarks. */
static void test_compact_watermarks(struct io_uring_opaque_config cfg)
{
	const size_t big = 1U << 20, step = 128U << 10;
	unsigned char *data = calloc(1, big);
	struct io_uring_opaque_stat before, after;
	struct event event;
	struct ring r;
	uint32_t context;
	uint64_t stream, large, offset = 0;
	int pair[2];

	cfg.hard_limit = 8U << 20;
	cfg.compact_headroom = 2U << 20;
	cfg.reply_reserve = 256U << 10;
	cfg.high_watermark = 4U << 20;
	cfg.low_watermark = 2U << 20;
	require(data && !ring_init(&r, IORING_SETUP_CQE32) &&
		!register_store(&r, &cfg, &context), "compaction watermark store");
	tcp_pair(pair);
	stream = attach(&r, context, pair[1], next_tag++);
	large = receive_complete(&r, context, stream, pair[0], offset, data, big).extra[0];
	offset += big;
	for (;;) {
		require(!command(&r, context, IORING_OPAQUE_STAT, 0, 0, sizeof(before),
				 &before).res, "compaction watermark stat");
		if (before.backing_bytes + big >= cfg.high_watermark + step)
			break;
		event = receive_complete(&r, context, stream, pair[0], offset, data, step);
		require(!event.extra[1], "cacheable object below the high watermark");
		offset += step;
	}
	require(before.memory_mode == IORING_OPAQUE_MEMORY_NORMAL, "normal before compaction");
	require(!command(&r, context, IORING_OPAQUE_COMPACT, large, 0, 0, NULL).res,
		"compact a large object near the high watermark");
	require(!command(&r, context, IORING_OPAQUE_STAT, 0, 0, sizeof(after), &after).res &&
		after.backing_bytes <= before.backing_bytes &&
		after.memory_mode == IORING_OPAQUE_MEMORY_NORMAL,
		"compaction copy does not switch the store to lowmem");
	event = receive_complete(&r, context, stream, pair[0], offset, "after!!!", 8);
	require(!event.extra[1], "object received after compaction is cacheable");
	require(!command(&r, context, IORING_OPAQUE_STREAM_CLOSE, stream, 0, 0, NULL).res,
		"compaction watermark close");
	ring_exit(&r);
	close(pair[0]);
	close(pair[1]);
	free(data);
	ksft_test_result_pass("compaction copies do not switch the store to lowmem\n");
}

/* A nonzero reply reserve is a floor: blocked replies cannot starve a healthy one. */
static void test_reply_reserve_floor(struct io_uring_opaque_config cfg)
{
	static unsigned char prefix[3000];
	struct io_uring_opaque_frame big = { .size = sizeof(big),
		.prefix = (uintptr_t)prefix, .prefix_length = sizeof(prefix) };
	struct io_uring_opaque_frame small = { .size = sizeof(small),
		.prefix = (uintptr_t)"HDR:", .prefix_length = 4 };
	const size_t length = 1U << 20, total = length + sizeof(prefix);
	unsigned char *data = malloc(length), *seen1 = malloc(total), *seen2 = malloc(total);
	struct slow_transfer rx1 = { .data = seen1, .length = total };
	struct slow_transfer rx2 = { .data = seen2, .length = total };
	struct io_uring_opaque_stat stat;
	unsigned char wire[10];
	pthread_t reader1, reader2;
	unsigned int i;
	struct ring r;
	uint32_t context;
	uint64_t stream, object, reply, first, second, healthy;
	int source[2], slow1[2], slow2[2], target[2];

	require(data && seen1 && seen2, "reply floor buffers");
	memset(prefix, 'P', sizeof(prefix));
	memset(data, 'D', length);
	cfg.hard_limit = 8U << 20;
	cfg.compact_headroom = 1U << 20;
	cfg.reply_reserve = 8U << 10;
	require(!ring_init(&r, IORING_SETUP_CQE32) && !register_store(&r, &cfg, &context),
		"reply floor store");
	tcp_pair(source);
	tcp_pair_small(slow1);
	tcp_pair_small(slow2);
	tcp_pair(target);
	stream = attach(&r, context, source[1], next_tag++);
	object = receive_complete(&r, context, stream, source[0], 0, data, length).extra[0];
	reply = receive_complete(&r, context, stream, source[0], length, "reply!", 6).extra[0];
	/* Two replies block on 4 KiB peers, holding 8 KiB of framing. */
	first = frame_stage(&r, context, slow1[0], object, length, &big, 0, 0);
	second = frame_stage(&r, context, slow2[0], object, length, &big, 0, 0);
	submit_only(&r);
	usleep(100000);
	require(!command(&r, context, IORING_OPAQUE_STAT, 0, 0, sizeof(stat), &stat).res &&
		stat.framing_bytes == 2 * 4096, "two blocked replies hold framing");
	healthy = frame_stage(&r, context, target[0], reply, 6, &small, 0, 0);
	require(wait_tag_ms(&r, healthy, 5000).res == (int)sizeof(wire),
		"healthy reply beside blocked replies");
	read_all(target[1], wire, sizeof(wire));
	require(!memcmp(wire, "HDR:reply!", sizeof(wire)), "healthy reply wire bytes");
	rx1.fd = slow1[1];
	rx2.fd = slow2[1];
	require(!pthread_create(&reader1, NULL, slow_reader, &rx1) &&
		!pthread_create(&reader2, NULL, slow_reader, &rx2), "slow reply readers");
	require(wait_tag_ms(&r, first, 30000).res == (int)total &&
		wait_tag_ms(&r, second, 30000).res == (int)total, "blocked replies complete");
	pthread_join(reader1, NULL);
	pthread_join(reader2, NULL);
	require(rx1.done == total && rx2.done == total &&
		!memcmp(seen1, prefix, sizeof(prefix)) &&
		!memcmp(seen1 + sizeof(prefix), data, length) &&
		!memcmp(seen2, seen1, total), "blocked reply wire bytes");
	require(!command(&r, context, IORING_OPAQUE_STREAM_CLOSE, stream, 0, 0, NULL).res,
		"reply floor close");
	/* A send's framing is released when its request is freed, just after its CQE. */
	for (i = 0; i < 1000; i++) {
		require(!command(&r, context, IORING_OPAQUE_STAT, 0, 0, sizeof(stat), &stat).res,
			"reply floor stat");
		if (!stat.framing_bytes)
			break;
		usleep(1000);
	}
	require(!stat.framing_bytes, "reply floor framing drains");
	ring_exit(&r);
	close(source[0]);
	close(source[1]);
	close(slow1[0]);
	close(slow1[1]);
	close(slow2[0]);
	close(slow2[1]);
	close(target[0]);
	close(target[1]);
	free(data);
	free(seen1);
	free(seen2);
	ksft_test_result_pass("a reply reserve is a floor for framing, not a cap\n");
}

static size_t drain_idle(int fd, unsigned char *buf, size_t cap, int idle_ms)
```

### 4.3 `test_reply_reserve()`: the exhaustion frame must exceed both pools

With floor semantics, a frame of `reply_reserve + 1` bytes can fit in leftover capture
capacity, so this check became timing-dependent; it failed one of my prototype's
full-suite runs. `IORING_OPAQUE_FRAME_MAX` (64 KiB) always exceeds the free capture capacity plus the
4 KiB reserve after the flood.

Old (`tools/testing/selftests/io_uring/opaque_obj.c`):

```c
	{
		struct io_uring_opaque_frame large = { .size = sizeof(large),
			.prefix = (uintptr_t)flood, .prefix_length = cfg.reply_reserve + 1 };

		tag = frame_stage(&r, context, target[0], request, 8, &large,
				  IORING_OPAQUE_SEND_LAST, 0);
		event = wait_tag_ms(&r, tag, 5000);
		require(event.res == -ENOBUFS && !(event.flags & IORING_CQE_F_OPAQUE_CONSUMED),
			"reply reserve exhaustion preserves LAST ownership");
	}
```

New:

```c
	{
		/* Framing larger than free capture capacity and the reserve together. */
		struct io_uring_opaque_frame large = { .size = sizeof(large),
			.prefix = (uintptr_t)flood, .prefix_length = IORING_OPAQUE_FRAME_MAX };

		tag = frame_stage(&r, context, target[0], request, 8, &large,
				  IORING_OPAQUE_SEND_LAST, 0);
		event = wait_tag_ms(&r, tag, 5000);
		require(event.res == -ENOBUFS && !(event.flags & IORING_CQE_F_OPAQUE_CONSUMED),
			"framing quota exhaustion preserves LAST ownership");
	}
```

### 4.4 Plan and calls in `main()`

Old (`tools/testing/selftests/io_uring/opaque_obj.c`):

```c
	ksft_set_plan(94);
```

New:

```c
	ksft_set_plan(97);
```

Old (`tools/testing/selftests/io_uring/opaque_obj.c`):

```c
	test_lowmem(cfg);
```

New:

```c
	test_lowmem(cfg);
	test_compact_watermarks(cfg);
```

Old (`tools/testing/selftests/io_uring/opaque_obj.c`):

```c
	test_reply_reserve(cfg);
```

New:

```c
	test_reply_reserve(cfg);
	test_reply_reserve_floor(cfg);
```

Old (`tools/testing/selftests/io_uring/opaque_obj.c`):

```c
	test_gap_after_decision(cfg);
```

New:

```c
	test_gap_after_decision(cfg);
	test_reinspect_same_offset(cfg);
```

---

## 5. Documentation, patch 5/5: `Documentation/userspace-api/io_uring-opaque.rst`

### 5.1 INSPECT section

Old (`Documentation/userspace-api/io_uring-opaque.rst`):

```rst
For example, INSPECT with a 512-byte destination returns a received 22-byte GET;
it does not wait for 490 unrelated future bytes. An incomplete header is parsed
incrementally: inspect its next missing offset and resume on that CQE. Arming
```

New:

```rst
For example, INSPECT with a 512-byte destination returns a received 22-byte GET;
it does not wait for 490 unrelated future bytes. An incomplete header is parsed
incrementally: inspect its next missing offset and resume on that CQE. While a
stream holds undecided bytes, inspecting the same offset again returns only the
bytes already collected, however large the destination; only an INSPECT that
waits past them makes the collector fetch more. Arming
```

### 5.2 Framing accounting

Old (`Documentation/userspace-api/io_uring-opaque.rst`):

```rst
Framing snapshots use accounted allocations in the store owner's memory cgroup.
Their allocator bucket size is charged until request cleanup. With a nonzero
``reply_reserve``, framing uses that separate allowance, which payload capture
and compaction cannot borrow. Capture may use
``hard_limit - compact_headroom - reply_reserve``; replacement reservations
may use ``hard_limit - reply_reserve``. Snapshot quota failure returns
ENOBUFS before admission. STAT exposes current ``framing_bytes`` and cumulative
``framing_copied_bytes`` separately from retained backing and capture copies;
``backing_bytes`` includes the framing charge. Per-request framing and pending
request bounds also apply to generated-only replies.
```

New:

```rst
Framing snapshots use accounted allocations in the store owner's memory cgroup.
Their allocator bucket size is charged until request cleanup. Capture may use
``hard_limit - compact_headroom - reply_reserve``; replacement reservations
may use ``hard_limit - reply_reserve``. Framing first uses the capture
allowance and takes the separate ``reply_reserve`` allowance, which payload
capture and compaction cannot borrow, only when capture capacity is full. A
nonzero reply reserve is therefore a floor for framing, not a cap. Snapshot
quota failure returns ENOBUFS before admission. STAT exposes current
``framing_bytes`` and cumulative ``framing_copied_bytes`` separately from
retained backing and capture copies; ``backing_bytes`` includes the framing
charge. Per-request framing and pending request bounds also apply to
generated-only replies.
```

Old (`Documentation/userspace-api/io_uring-opaque.rst`):

```rst
Concurrent snapshots can still exhaust the reply reserve, and allocation or
request admission can fail independently. Framing cannot consume compaction
headroom. With a zero reply reserve, snapshots share ordinary capture
capacity, so an eviction guarantees reply admission only while no permitted
collection pass takes the freed capacity first.
```

New:

```rst
Concurrent snapshots can still exhaust the reply reserve once capture capacity
is full, and allocation or request admission can fail independently. Framing
cannot consume compaction headroom. With a zero reply reserve, snapshots share
ordinary capture capacity, so an eviction guarantees reply admission only
while no permitted collection pass takes the freed capacity first.
```

### 5.3 Watermarks

Old (`Documentation/userspace-api/io_uring-opaque.rst`):

```rst
With a nonzero high watermark, STAT reports ``memory_mode`` as
``IORING_OPAQUE_MEMORY_NORMAL`` or ``IORING_OPAQUE_MEMORY_LOWMEM``. Payload
backing and compaction reservations reaching the high watermark enter lowmem;
return to normal occurs at or below the low watermark. Protected framing
snapshots do not change the mode. Importing rings observe the same store mode.
```

New:

```rst
With a nonzero high watermark, STAT reports ``memory_mode`` as
``IORING_OPAQUE_MEMORY_NORMAL`` or ``IORING_OPAQUE_MEMORY_LOWMEM``. The
watermarks measure payload held: cached objects, undecided bytes and objects
being assembled. Payload reaching the high watermark enters lowmem; return to
normal occurs at or below the low watermark. A compaction copy is not counted
until its request completes, so a compaction that lowers occupancy cannot
enter lowmem. Framing snapshots never change the mode. Importing rings
observe the same store mode.
```

### 5.4 Forwarding-only objects

The third paragraph of this section, starting "Forwarding still needs temporary backing",
stays unchanged.

Old (`Documentation/userspace-api/io_uring-opaque.rst`):

```rst
RECV_OBJECT samples the mode when reserving its object slot. Normal admission
creates a cacheable object, even if its later assembly crosses the high
watermark. Lowmem admission creates a forwarding-only object automatically.
Headers remain available through INSPECT, and DISCARD can skip future bodies
without backing. The complete-object notification reports the classification;
OBJECT_STAT also reports ``IORING_OPAQUE_OBJECT_F_FORWARD_ONLY``.

Existing cacheable objects keep their ordinary SEND behavior during lowmem.
A forwarding-only object must use SEND_LAST for transmission, or FREE to
discard its completed backing. Ordinary SEND returns EOPNOTSUPP without
consuming it or transmitting bytes. READ_OBJECT remains a bounded copy;
neither a return to normal nor manual compaction makes the handle cacheable.
Automatic compaction excludes forwarding-only objects. No new syscall or
application submission flag is required.
```

New:

```rst
RECV_OBJECT samples the mode when reserving its object slot. Normal admission
creates a cacheable object, even if its later assembly crosses the high
watermark. Lowmem admission marks the object forwarding-only. Headers remain
available through INSPECT, and DISCARD can skip future bodies without
backing. The complete-object notification reports the classification;
OBJECT_STAT also reports ``IORING_OPAQUE_OBJECT_F_FORWARD_ONLY``.

The classification is advisory. It tells the application that the object
arrived under memory pressure and should be forwarded and released promptly,
with SEND_LAST or FREE. The kernel does not restrict how the object is sent:
ordinary SEND, fan-out to several destinations and retry after a failed send
remain available, and READ_OBJECT remains a bounded copy. The classification
survives compaction and a return to normal mode. Automatic compaction skips
forwarding-only objects; manual compaction does not. No new syscall or
application submission flag is required.
```

### 5.5 Test summaries

Old (`Documentation/userspace-api/io_uring-opaque.rst`):

```rst
inspection, forwarding-only ownership, stable classification through compaction,
retained undecided bytes and held collection, split headers collected by a
waiting INSPECT, a decision past an uncollected gap, a flooding stream that
cannot refill freed capacity, and protected replies when one collection pass
fills the store.
```

New:

```rst
inspection, advisory forwarding-only sends, stable classification through
compaction, compaction near the high watermark, retained undecided bytes and
held collection, split headers collected by a waiting INSPECT, same-offset
re-inspection, a decision past an uncollected gap, a flooding stream that
cannot refill freed capacity, protected replies when one collection pass
fills the store, and replies beside framing blocked on slow peers.
```

Old (`Documentation/userspace-api/io_uring-opaque.rst`):

```rst
Memory-policy cases cover watermark transitions, separate reply accounting,
the collection gate (retained undecided bytes, held passes and decided bytes
through a held gate), and forwarding-only version replacement and send
admission.
```

New:

```rst
Memory-policy cases cover watermark transitions, compaction copies excluded
from the watermarks, framing that takes the reply reserve only after capture
capacity, the collection gate (retained undecided bytes, held passes and
decided bytes through a held gate), and forwarding-only version replacement
with advisory send admission.
```

Change nothing else in the document.

---

## 6. Commit messages (final text, verbatim)

- **Patch 1/5 is not touched.** It keeps SHA `fc0538f7d01a`.
- **Patches 2/5 to 5/5 get exactly the messages below,** byte for byte, including both
  trailers. `git log -1 --format=%B <commit>` must print the block followed by one empty
  line.
- **Don't add or remove trailers.**

### 2/5

```text
io_uring: add opaque TCP stores, framed sends and lowmem forwarding

Register hardware-independent stores through native io_uring. Collect
eligible ordinary TCP backing by reference with bounded copy fallback;
applications own stream offsets, parsing, object boundaries and policy.

INSPECT returns available contiguous framing and arms asynchronously when
none exists. RECV_OBJECT privately assembles a declared complete range and
publishes one handle/readiness CQE; READ_OBJECT copies completed bytes.
Restore unsuccessful private prefixes under the stream lock before their
completion without publishing partial handles.

Collect only bytes the application can decide. A collection pass may take
everything queued when the stream has no undecided bytes; undecided bytes
stay in their receive buffers and only INSPECT copies them. While a stream
has undecided bytes, later bytes stay on the socket under TCP flow control
unless an INSPECT waits past the collected bytes or a decision starts
beyond them. Decided bytes are always collected, and a decision that
becomes the earliest during a pass collects the gap before it.

SEND covers a bounded copied prefix, object subrange and copied suffix
with one request, FIFO position, continuation cursor and progress CQE.
Generated-only replies share that queue. Snapshot, validation and quota
failures precede admission; ordinary sends pin a version and LAST transfers
the handle reference with an independent consumed flag. Only a send that
attempted TCP can fail admitted followers. Canceling an unstarted follower
must preserve its successors regardless of head/follower cleanup order.

Reserve before TCP capture, report nonterminal ENOBUFS with the token and
unread offset, and resume on eligible capacity release. Unadmitted bytes
remain on the socket; known DISCARD ranges can skip without backing.
Bound backing, allocator-bucket framing, metadata and pending requests.

Offer creation-time high/low watermarks over the payload held:
unpublished compaction copies and reply framing are excluded, and a
seqcount keeps that subtraction consistent with the charge. Reply framing
uses ordinary capacity first and the protected reply reserve only when
that is full. Lowmem receive admissions mark objects forwarding-only; the
classification is advisory, is reported in completions and OBJECT_STAT,
and survives compaction and mode changes. Temporary object assembly and
forwarding still obey the hard limit. Zero new fields retain the legacy
shared allowance.

Resolve handles at first issue and preserve old-version references across
FREE/LAST and manual or automatic compaction. Stores support descriptor
export/import and size-versioned registration, queries and policy. No
zcrx, mapped-area, page-pool or busy-poll dependency is introduced.

One delayed compaction dispatcher serializes manual and automatic copies.
Prune ineligible candidates, re-enroll on policy changes and coalesce
publication wakeups without resetting the paced scan deadline. Forwarding
objects are excluded from automatic compaction. Keep allocation retry
independent and drain accepted manual requests during shutdown.

Assisted-by: Codex:GPT-6
Assisted-by: GLM:glm-5.3
```

### 3/5

```text
io_uring: test opaque ownership, memory modes and compaction

Cover RX identity and copy exceptions, shared charges, capture bounds,
range rollback, generation retirement and stable-handle replacement.
Verify available inspection and reusable/LAST version references through
compaction races without restoring consumed handles.

Add watermark hysteresis, compaction copies excluded from the
watermarks, and reply framing that takes the reserve only after ordinary
capacity. Check the collection gate: retained undecided bytes are not
copied, a held pass takes no undecided bytes, and decided bytes pass a
held gate. Verify forwarding-only classification survives replacement
while ordinary and final sends are both admitted. The suite has 28 KUnit
cases.

Introduce the KUnit option and build rule with the test source so enabling
KUNIT_ALL_TESTS cannot break the preceding implementation patch.

Assisted-by: Codex:GPT-6
Assisted-by: GLM:glm-5.3
```

### 4/5

```text
selftests/io_uring: exercise opaque lowmem, quota and SMP cancellation

Use raw 64-byte SQEs and CQE32/MIXED rings without liburing. Cover short
GET availability and arming races, declared future objects, complete
prefix/body/suffix sends, reusable/LAST ownership, generated-only replies,
snapshot faults and lifetime, compaction, backpressure and partial heads.

Cancel waiting followers before and after head cleanup on one and shared
rings; verify successors remain sendable and consumed handles stay stale.
Exercise watermark validation and hysteresis, compaction near the high
watermark, lowmem header inspection, direct DISCARD, advisory
forwarding-only sends, protected framing when one collection pass fills
the store, and a healthy reply beside framing blocked on slow peers.

Check the collection policy: undecided bytes are retained without copies
and hold later bytes on the socket until a decision; a waiting INSPECT
collects a split header, while re-inspecting an offset returns only bytes
already collected; a decision past an uncollected gap completes without
another arrival; a flooding stream cannot refill capacity freed by an
eviction; and INSPECT sees a header longer than one page.

Keep 10,000 cross-ring SEND cancellation/promotion races with issuing rings
on different CPUs. Verify complete and partial heads, explicit cancel and
linked timeouts, unique completions, wire order and reference drain. Keep
10,000 separate receive arrival/drain cycles and 4 GiB DISCARD through TCP
sequence wrap. These establish correctness, not performance or measured
scheduler-wakeup counts.

Cover blocked IPv4/IPv6 sends during rejected disconnect, same-batch
receive cancellation and inspection, pressure coalescing and unread TCP
bytes, cross-ring resume, CQ pressure, versioned UAPI and fault injection.
The suite has 97 groups. Make its alarm configurable through
OPAQUE_TEST_TIMEOUT and propagate it through the diagnostic VM helper.

Assisted-by: Codex:GPT-6
Assisted-by: GLM:glm-5.3
```

### 5/5

```text
docs: describe opaque forwarding, memory and ownership contracts

Specify store sharing, available asynchronous inspection, complete-object
readiness and immutable reads. Document one-command framed SEND, LAST
admission and its independent consumed indication, partial progress and
queued-follower cancellation semantics.

Explain the collection policy: what one pass may take, retained undecided
bytes, held collection and its two demand exceptions, EOF behind held
bytes, and same-offset re-inspection. Explain optional high/low watermarks
over the payload held, a reply reserve that is a floor for framing, and
advisory forwarding-only classification that survives replacement and
mode transitions. Describe actual backing charges, including
whole-allocation charges for retained undecided bytes, temporary
forwarding capacity and hard-limit backpressure without promising
unbounded header or forwarding progress.

Document nonterminal pressure, reservation before capture, coalescing and
resume conditions, and terminal CQ failure without a collector rearm API.
Explain stable-handle compaction, candidate pruning, paced publication
wakeups, worker lifetime and reproducible debug VM testing with adjustable
test and host timeouts.

Keep prototype provenance, performance hypotheses and baseline discussion
in the cover letter and separate research records.

Assisted-by: Codex:GPT-6
Assisted-by: GLM:glm-5.3
```

---

## 7. Pitfalls

1. **The watermark check reads `bytes` and `transient` inside the seqcount loop**
   (`read_seqcount_begin`/`read_seqcount_retry`).
   - Never update `transient` outside a `transient_lock` + `write_seqcount_begin/end`
     section.
   - Never call `io_opaque_memory_mode()`, `io_opaque_charge()`,
     `io_opaque_uncharge()` or `io_opaque_wake_budget()` inside that section.
   - The helpers in §2.5 do this correctly; reuse them.
2. **Compaction no longer calls `io_opaque_charge(..., false)`.**
   - `io_opaque_compact_end()` releases the leftover `op->reserved` itself.
   - Remove the old `io_opaque_uncharge(store, op->reserved)` calls exactly as §2.8 and
     §2.11 show; keeping them double-uncharges.
3. **Call `io_opaque_compact_end()` only after** the request's references to `op->data`
   and `op->replacement` are dropped, as the blocks show.
4. **Initialize `transient_lock` and `transient_seq`** in both `io_opaque_alloc()` and the
   KUnit `opaque_test_store()`. Lockdep reports an uninitialized seqcount lock otherwise.
5. **Framing records its pool in `op->frame_reserved`.** Pass it to
   `io_opaque_uncharge_frame()`. Don't recompute it from the current configuration.
6. **Remove only the `-EOPNOTSUPP` check for forwarding-only objects.** Everything else
   about them stays.
7. **In `test_reply_reserve_floor()`, keep the polling loop at the end.** A send's framing
   is released when io_uring frees the request, which can happen just after its CQE has
   been reaped (the prototype saw up to one poll of lag).
8. **Diagnostics are reported, never suppressed.**
   - Quote and attribute every `BUG:`, `WARNING:`, `KASAN:`, `refcount_t:`, lockdep or
     kmemleak report from its backtrace.
   - Never disable a detector to make a report go away.
   - The acceptance config does not need `DEBUG_KMEMLEAK`.
   - KUnit runs at boot without `kunit.enable=1`, because `CONFIG_KUNIT_DEFAULT_ENABLED=y`.

---

## 8. Acceptance criteria

1. **Shape.** `git range-diff af32da41b032..19e616d733c8 af32da41b032..HEAD` shows:
   - 1/5 `=`;
   - 2/5: §2 plus the message;
   - 3/5: §3 plus the message;
   - 4/5: §4 plus the message;
   - 5/5: §5 plus the message.
2. **Builds.**
   - Every commit with `CONFIG_IO_URING_OPAQUE_OBJ=y`,
     `CONFIG_IO_URING_OPAQUE_OBJ_KUNIT_TEST=y` and `CONFIG_KUNIT_ALL_TESTS=y`, forced on
     where the symbol exists.
   - `W=1` clean on `io_uring/` and the changed `net/` objects.
   - `CONFIG_IO_URING_OPAQUE_OBJ=n`.
   - Native `ARCH=i386` with `HIGHMEM4G`.
   - The static selftest builds without warnings under `-Wall -Wextra`.
3. **checkpatch.** `scripts/checkpatch.pl --strict` reports only the missing
   `Signed-off-by` and the MAINTAINERS file-path warnings.
4. **KUnit.** `# Totals: pass:28 fail:0 skip:0 total:28`, including
   `ok ... opaque_compact_watermarks`.
5. **Selftests.** `# Totals: pass:97 fail:0 xfail:0 xpass:0 skip:0 error:0` under KASAN,
   lockdep, PREEMPT and 2 vCPUs, with the `SEND races:` line printed and no kernel
   diagnostics.
6. **Fails before the fix.** Run the new selftest binary against the round-1 kernel,
   `/mnt/workspace/opaque-vm/v9/build-kernel/linux` (release
   `7.3.0-rc6-g19e616d733c8`). It must print `ok 46 lowmem parses short headers ...`
   and then
   `Bail out! forwarding-only classification is advisory for SEND`.
7. **Text checks.** Each must print nothing:

   ```sh
   grep -n -e 'EOPNOTSUPP without' -e 'must use SEND_LAST' -e 'compaction reservations reaching' \
        -e 'forwarding-only ownership' Documentation/userspace-api/io_uring-opaque.rst
   git log --format=%B af32da41b032..HEAD | grep -n -i -e 'forbids reusable' \
        -e '94 groups' -e '27 KUnit' -e 'immutable forwarding-only'
   grep -n 'forward_only && !(op->send_flags' io_uring/opaque.c
   ```
