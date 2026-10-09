// SPDX-License-Identifier: GPL-2.0
/* Compaction is added separately from the receive and transmit interface. */
static void io_opaque_auto_work(struct work_struct *work)
{
}

static int io_opaque_compact_start(struct io_opaque_req *op)
{
	return -EOPNOTSUPP;
}

static int io_opaque_set_policy(struct io_opaque_req *op)
{
	return -EOPNOTSUPP;
}

static int io_opaque_compact_publish(struct io_opaque_req *op)
{
	return -EOPNOTSUPP;
}
