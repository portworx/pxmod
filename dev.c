/*
  FUSE: Filesystem in Userspace
  Copyright (C) 2001-2008  Miklos Szeredi <miklos@szeredi.hu>

  This program can be distributed under the terms of the GNU GPL.
  See the file COPYING.
*/

#include "fuse_i.h"

#include <linux/init.h>
#include <linux/module.h>
#include <linux/poll.h>
#include <linux/uio.h>
#include <linux/miscdevice.h>
#include <linux/pagemap.h>
#include <linux/file.h>
#include <linux/slab.h>
#include <linux/pipe_fs_i.h>
#include <linux/swap.h>
#include <linux/splice.h>
#include <linux/aio.h>
#include <linux/random.h>
#include <linux/version.h>
#include <linux/blkdev.h>
#include <linux/sort.h>
#include "pxd_compat.h"

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,6,0)
#define PAGE_CACHE_GET(page) get_page(page)
#define PAGE_CACHE_RELEASE(page) put_page(page)
#else
#define PAGE_CACHE_GET(page) page_cache_get(page)
#define PAGE_CACHE_RELEASE(page) page_cache_release(page)
#endif

/** Maximum number of outstanding background requests */
#define FUSE_DEFAULT_MAX_BACKGROUND (PXD_MAX_QDEPTH * PXD_MAX_DEVICES)

#define FUSE_MAX_REQUEST_IDS (2 * FUSE_DEFAULT_MAX_BACKGROUND)

static struct kmem_cache *fuse_req_cachep;

static struct fuse_conn *fuse_get_conn(struct file *file)
{
	/*
	 * Lockless access is OK, because file->private data is set
	 * once during mount and is valid until the file is released.
	 */
	return file->private_data;
}

void fuse_request_init(struct fuse_req *req)
{
	memset(req, 0, sizeof(*req));
}

static struct fuse_req *__fuse_request_alloc(gfp_t flags)
{
	struct fuse_req *req = kmem_cache_alloc(fuse_req_cachep, flags);

	if (req) {
		fuse_request_init(req);
	}

	return req;
}

struct fuse_req *fuse_request_alloc()
{
	return __fuse_request_alloc(GFP_NOIO);
}

struct fuse_req *fuse_request_alloc_nofs()
{
	return __fuse_request_alloc(GFP_NOFS);
}

void fuse_request_free(struct fuse_req *req)
{
	kmem_cache_free(fuse_req_cachep, req);
}

void fuse_req_init_context(struct fuse_req *req)
{
	req->in.h.uid = from_kuid_munged(&init_user_ns, current_fsuid());
	req->in.h.gid = from_kgid_munged(&init_user_ns, current_fsgid());
	req->in.h.pid = current->pid;
}

static struct fuse_req *__fuse_get_req(struct fuse_conn *fc)
{
	struct fuse_req *req;
	int err;

	if (!fc->connected && !fc->allow_disconnected) {
		 err = -ENOTCONN;
		goto out;
	}

	req = fuse_request_alloc();
	if (!req) {
		err = -ENOMEM;
		goto out;
	}

	fuse_req_init_context(req);
	return req;

 out:
	return ERR_PTR(err);
}

struct fuse_req *fuse_get_req(struct fuse_conn *fc)
{
	return __fuse_get_req(fc);
}

struct fuse_req *fuse_get_req_for_background(struct fuse_conn *fc)
{
	return __fuse_get_req(fc);
}

static unsigned len_args(unsigned numargs, struct fuse_arg *args)
{
	unsigned nbytes = 0;
	unsigned i;

	for (i = 0; i < numargs; i++)
		nbytes += args[i].size;

	return nbytes;
}

static u64 fuse_get_unique(struct fuse_conn *fc)
{
	struct fuse_per_cpu_ids *my_ids;
	u64 uid;
	int num_alloc;

	int cpu = get_cpu();

	my_ids = per_cpu_ptr(fc->per_cpu_ids, cpu);

	if (unlikely(my_ids->num_free_ids == 0)) {
		spin_lock(&fc->lock);
		BUG_ON(fc->num_free_ids == 0);
		num_alloc = min(fc->num_free_ids, (u32)FUSE_MAX_PER_CPU_IDS / 2);
		memcpy(my_ids->free_ids, &fc->free_ids[fc->num_free_ids - num_alloc],
			num_alloc * sizeof(u64));
		fc->num_free_ids -= num_alloc;
		spin_unlock(&fc->lock);

		my_ids->num_free_ids = num_alloc;
	}

	uid = my_ids->free_ids[--my_ids->num_free_ids];

	put_cpu();

	uid += FUSE_MAX_REQUEST_IDS;

	/* zero is special */
	if (uid == 0)
		uid += FUSE_MAX_REQUEST_IDS;

	return uid;
}

static void fuse_put_unique(struct fuse_conn *fc, u64 uid)
{
	struct fuse_per_cpu_ids *my_ids;
	int num_free;
	int cpu = get_cpu();

	my_ids = per_cpu_ptr(fc->per_cpu_ids, cpu);

	if (unlikely(my_ids->num_free_ids == FUSE_MAX_PER_CPU_IDS)) {
		num_free = FUSE_MAX_PER_CPU_IDS / 2;
		spin_lock(&fc->lock);
		BUG_ON(fc->num_free_ids + num_free > FUSE_MAX_REQUEST_IDS);
		memcpy(&fc->free_ids[fc->num_free_ids],
			&my_ids->free_ids[my_ids->num_free_ids - num_free],
			num_free * sizeof(u64));
		fc->num_free_ids += num_free;
		spin_unlock(&fc->lock);

		my_ids->num_free_ids -= num_free;
	}

	my_ids->free_ids[my_ids->num_free_ids++] = uid;

	fc->request_map[uid & (FUSE_MAX_REQUEST_IDS - 1)] = NULL;

	put_cpu();
}

static void queue_request(struct fuse_conn *fc, struct fuse_req *req)
{
	u32 write, next_index;

	spin_lock(&fc->queue.w.lock);
	write = fc->queue.w.write;
	next_index = (write + 1) & (FUSE_REQUEST_QUEUE_SIZE - 1);
	if (fc->queue.w.read == next_index) {
		fc->queue.w.read = fc->queue.r.read;
		BUG_ON(next_index == fc->queue.w.read);
	}

	fc->queue.w.requests[write] = req;
	req->sequence = fc->queue.w.sequence++;
	smp_wmb();
	fc->queue.w.write = next_index;
	spin_unlock(&fc->queue.w.lock);
}

static void fuse_conn_wakeup(struct fuse_conn *fc)
{
	wake_up(&fc->waitq);
	kill_fasync(&fc->fasync, SIGIO, POLL_IN);
}

/*
 * This function is called when a request is finished.  Either a reply
 * has arrived or it was aborted (and not yet sent) or some error
 * occurred during communication with userspace, or the device file
 * was closed.  The requester thread is woken up (if still waiting),
 * the 'end' callback is called if given, else the reference to the
 * request is released
 */
static void request_end(struct fuse_conn *fc, struct fuse_req *req)
{
	u64 uid = req->in.h.unique;
	if (req->end)
		req->end(fc, req);
	fuse_put_unique(fc, uid);
#ifndef __PX_BLKMQ__
	fuse_request_free(req);
#endif
}

void fuse_request_send_nowait(struct fuse_conn *fc, struct fuse_req *req)
{
	req->in.h.len = sizeof(struct fuse_in_header) +
		len_args(req->in.numargs, (struct fuse_arg *)req->in.args);

	req->in.h.unique = fuse_get_unique(fc);
	fc->request_map[req->in.h.unique & (FUSE_MAX_REQUEST_IDS - 1)] = req;

	spin_lock(&fc->lock);

	if (fc->connected || fc->allow_disconnected) {
		spin_unlock(&fc->lock);
		queue_request(fc, req);
		fuse_conn_wakeup(fc);
	} else {
		req->out.h.error = -ENOTCONN;
		spin_unlock(&fc->lock);
		request_end(fc, req);
	}
}

static int request_pending(struct fuse_conn *fc)
{
	u32 wwrite;
	/* check cached value first */
	if (fc->queue.r.read != fc->queue.r.write)
		return true;
	/* check the writer value, if it is same nothing pending */
	wwrite = fc->queue.w.write;
	if (fc->queue.r.read == wwrite)
		return false;
	/* update cache with new value */
	fc->queue.r.write = wwrite;
	return true;
}

/* Wait until a request is available on the pending list */
static void request_wait(struct fuse_conn *fc)
__releases(fc->lock)
__acquires(fc->lock)
{
	DECLARE_WAITQUEUE(wait, current);

	add_wait_queue_exclusive(&fc->waitq, &wait);
	while (fc->connected && !request_pending(fc)) {
		set_current_state(TASK_INTERRUPTIBLE);
		if (signal_pending(current))
			break;

		spin_unlock(&fc->lock);
		schedule();
		spin_lock(&fc->lock);
	}
	set_current_state(TASK_RUNNING);
	remove_wait_queue(&fc->waitq, &wait);
}

ssize_t fuse_copy_req_read(struct fuse_req *req, struct iov_iter *iter)
{
	size_t copied, len;

	copied = sizeof(req->in.h);
	if (copy_to_iter(&req->in.h, copied, iter) != copied) {
		printk(KERN_ERR "%s: copy header error\n", __func__);
		return -EFAULT;
	}

	len = req->in.args[0].size;
	if (copy_to_iter((void *)req->in.args[0].value, len, iter) != len) {
		printk(KERN_ERR "%s: copy arg error\n", __func__);
		return -EFAULT;
	}
	copied += len;

	return copied;
}

extern uint32_t pxd_detect_zero_writes;

/* Check if the request is writing zeroes and if so, convert it as a discard
 * request.
 */
static void fuse_convert_zero_writes(struct fuse_req *req)
{
	uint8_t wsize = sizeof(uint64_t);
	struct req_iterator breq_iter;
#ifdef HAVE_BVEC_ITER
	struct bio_vec bvec;
#else
	struct bio_vec *bvec = NULL;
#endif
	char *kaddr, *p;
	size_t i, len;
	uint64_t *q;

	rq_for_each_segment(bvec, req->rq, breq_iter) {
		kaddr = kmap_atomic(BVEC(bvec).bv_page);
		p = kaddr + BVEC(bvec).bv_offset;
		q = (uint64_t *)p;
		len = BVEC(bvec).bv_len;
		for (i = 0; i < (len / wsize); i++) {
			if (q[i]) {
				kunmap_atomic(kaddr);
				return;
			}
		}
		for (i = len - (len % wsize); i < len; i++) {
			if (p[i]) {
				kunmap_atomic(kaddr);
				return;
			}
		}
		kunmap_atomic(kaddr);
	}
	req->in.h.opcode = PXD_DISCARD;
}

/*
 * Read a single request into the userspace filesystem's buffer.  This
 * function waits until a request is available, then removes it from
 * the pending list and copies request data to userspace buffer.  If
 * no reply is needed (FORGET) or request has been aborted or there
 * was an error during the copying then it's finished by calling
 * request_end().  Otherwise add it to the processing list, and set
 * the 'sent' flag.
 */
static ssize_t fuse_dev_do_read(struct fuse_conn *fc, struct file *file,
	struct iov_iter *iter)
{
	int err;
	struct fuse_req *req;
	ssize_t copied = 0, copied_this_time;
	ssize_t remain = iter->count;
	u32 read, write;

	if (!request_pending(fc)) {
		if ((file->f_flags & O_NONBLOCK) && fc->connected)
			return -EAGAIN;
		spin_lock(&fc->lock);
		request_wait(fc);
		err = -ENODEV;
		if (!fc->connected)
			goto err_unlock;
		err = -ERESTARTSYS;
		if (!request_pending(fc))
			goto err_unlock;
		spin_unlock(&fc->lock);
	}

retry:
	read = fc->queue.r.read;
	write = fc->queue.r.write;

	while (read != write) {
		req = fc->queue.r.requests[read];

		if (req->in.h.len > remain)
			break;

		fc->queue.r.requests[read] = NULL;
		read = (read + 1) & (FUSE_REQUEST_QUEUE_SIZE - 1);

		/* Check if a write request is writing zeroes */
		if (pxd_detect_zero_writes && (req->in.h.opcode == PXD_WRITE) &&
		    req->misc.pxd_rdwr_in.size &&
		    !(req->misc.pxd_rdwr_in.flags & PXD_FLAGS_SYNC)) {
			fuse_convert_zero_writes(req);
		}

		copied_this_time = fuse_copy_req_read(req, iter);

		if (copied_this_time < 0) {
			req->out.h.error = -EIO;
			request_end(fc, req);
		} else {
			copied += copied_this_time;
			remain -= copied_this_time;
		}
	}

	fc->queue.r.read = read;

	/* Check if more requests could be picked up */
	if (remain && request_pending(fc))
		goto retry;

	return copied;

 err_unlock:
	spin_unlock(&fc->lock);
	return err;
}


#if LINUX_VERSION_CODE < KERNEL_VERSION(4,0,0)
static ssize_t fuse_dev_read(struct kiocb *iocb, const struct iovec *iov,
			      unsigned long nr_segs, loff_t pos)
{
	struct file *file = iocb->ki_filp;
	struct fuse_conn *fc = fuse_get_conn(file);
	struct iov_iter iter;
	if (!fc)
		return -EPERM;
	iov_iter_init(&iter, READ, iov, nr_segs, iov_length(iov, nr_segs));

	return fuse_dev_do_read(fc, file, &iter);
}
#else
static ssize_t fuse_dev_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
	struct file *file = iocb->ki_filp;
	struct fuse_conn *fc = fuse_get_conn(file);
	if (!fc)
		return -EPERM;

	return fuse_dev_do_read(fc, file, to);
}
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(3,14,0)
static int fuse_dev_pipe_buf_steal(struct pipe_inode_info *pipe,
                                   struct pipe_buffer *buf)
{
        return 1;
}
static const struct pipe_buf_operations fuse_dev_pipe_buf_ops = {
         .can_merge = 0,
         .map = generic_pipe_buf_map,
         .unmap = generic_pipe_buf_unmap,
         .confirm = generic_pipe_buf_confirm,
         .release = generic_pipe_buf_release,
         .steal = fuse_dev_pipe_buf_steal,
         .get = generic_pipe_buf_get,
};
#endif

static ssize_t fuse_dev_splice_read(struct file *in, loff_t *ppos,
				    struct pipe_inode_info *pipe,
				    size_t len, unsigned int flags)
{
	return -EINVAL;
}

static int fuse_notify_add(struct fuse_conn *conn, unsigned int size,
		struct iov_iter *iter)
{
	struct pxd_add_out add;
	size_t len = sizeof(add);

	if (copy_from_iter(&add, len, iter) != len) {
		printk(KERN_ERR "%s: can't copy arg\n", __func__);
		return -EFAULT;
	}
	return pxd_add(conn, &add);
}

/* Look up request on processing list by unique ID */
static struct fuse_req *request_find(struct fuse_conn *fc, u64 unique)
{
	u32 index = unique & (FUSE_MAX_REQUEST_IDS - 1);
	struct fuse_req *req = fc->request_map[index];
	if (req == NULL) {
		printk(KERN_ERR "no request unique %llx", unique);
		return req;
	}
	if (req->in.h.unique != unique) {
		printk(KERN_ERR "id mismatch got %llx need %llx", req->in.h.unique, unique);
		return NULL;
	}
	return req;
}

#define IOV_BUF_SIZE 64

static int copy_in_read_data_iovec(struct iov_iter *iter,
	struct pxd_read_data_out *read_data, struct iovec *iov,
	struct iov_iter *data_iter)
{
	int iovcnt;
	size_t len;

	if (!read_data->iovcnt)
		return -EFAULT;

	iovcnt = min(read_data->iovcnt, IOV_BUF_SIZE);
	len = iovcnt * sizeof(struct iovec);
	if (copy_from_iter(iov, len, iter) != len) {
		printk(KERN_ERR "%s: can't copy iovec\n", __func__);
		return -EFAULT;
	}
	read_data->iovcnt -= iovcnt;

	iov_iter_init(data_iter, READ, iov, iovcnt, iov_length(iov, iovcnt));

	return 0;
}

static int fuse_notify_read_data(struct fuse_conn *conn, unsigned int size,
				struct iov_iter *iter)
{
	struct pxd_read_data_out read_data;
	size_t len = sizeof(read_data);
	struct fuse_req *req;
	struct iovec iov[IOV_BUF_SIZE];
#ifdef HAVE_BVEC_ITER
	struct bio_vec bvec;
#else
	struct bio_vec *bvec = NULL;
#endif
	struct req_iterator breq_iter;
	struct iov_iter data_iter;
	size_t copied, skipped = 0;
	int ret;

	if (copy_from_iter(&read_data, len, iter) != len) {
		printk(KERN_ERR "%s: can't copy read_data arg\n", __func__);
		return -EFAULT;
	}

	req = request_find(conn, read_data.unique);
	if (!req) {
		printk(KERN_ERR "%s: request %lld not found\n", __func__,
		       read_data.unique);
		return -ENOENT;
	}

	if (req->in.h.opcode != PXD_WRITE &&
	    req->in.h.opcode != PXD_WRITE_SAME) {
		printk(KERN_ERR "%s: request is not a write\n", __func__);
		return -EINVAL;
	}

	ret = copy_in_read_data_iovec(iter, &read_data, iov, &data_iter);
	if (ret)
		return ret;

	/* advance the iterator if data is unaligned */
	if (unlikely(req->misc.pxd_rdwr_in.offset & PXD_LBS_MASK))
		iov_iter_advance(&data_iter,
				 req->misc.pxd_rdwr_in.offset & PXD_LBS_MASK);

	rq_for_each_segment(bvec, req->rq, breq_iter) {
		copied = 0;
		len = BVEC(bvec).bv_len;
		if (skipped < read_data.offset) {
			if (read_data.offset - skipped >= len) {
				skipped += len;
				copied = len;
			} else {
				copied = read_data.offset - skipped;
				skipped = read_data.offset;
			}
		}
		if (copied < len) {
			size_t copy_this = copy_page_to_iter(BVEC(bvec).bv_page,
				BVEC(bvec).bv_offset + copied,
				len - copied, &data_iter);
			if (copy_this != len - copied) {
				if (!iter->count)
					return 0;

				/* out of space in destination, copy more iovec */
				ret = copy_in_read_data_iovec(iter, &read_data,
					iov, &data_iter);
				if (ret)
					return ret;
				len -= copied;
				copied = copy_page_to_iter(BVEC(bvec).bv_page,
					BVEC(bvec).bv_offset + copied + copy_this,
					len, &data_iter);
				if (copied != len) {
					printk(KERN_ERR "%s: copy failed new iovec\n",
						__func__);
					return -EFAULT;
				}
			}
		}
	}

	return 0;
}

static int fuse_notify_remove(struct fuse_conn *conn, unsigned int size,
		struct iov_iter *iter)
{
	struct pxd_remove_out remove;
	size_t len = sizeof(remove);

	if (copy_from_iter(&remove, len, iter) != len) {
		printk(KERN_ERR "%s: can't copy arg\n", __func__);
		return -EFAULT;
	}
	return pxd_remove(conn, &remove);
}

static int fuse_notify_update_size(struct fuse_conn *conn, unsigned int size,
		struct iov_iter *iter)
{
	struct pxd_update_size_out update_size;
	size_t len = sizeof(update_size);

	if (copy_from_iter(&update_size, len, iter) != len) {
		printk(KERN_ERR "%s: can't copy arg\n", __func__);
		return -EFAULT;
	}
	return pxd_update_size(conn, &update_size);
}

static int fuse_notify(struct fuse_conn *fc, enum fuse_notify_code code,
		       unsigned int size, struct iov_iter *iter)
{
	switch ((int)code) {
	case PXD_READ_DATA:
		return fuse_notify_read_data(fc, size, iter);
	case PXD_ADD:
		return fuse_notify_add(fc, size, iter);
	case PXD_REMOVE:
		return fuse_notify_remove(fc, size, iter);
	case PXD_UPDATE_SIZE:
		return fuse_notify_update_size(fc, size, iter);
	default:
		return -EINVAL;
	}
}

/*
 * Write a single reply to a request.  First the header is copied from
 * the write buffer.  The request is then searched on the processing
 * list by the unique ID found in the header.  If found, then remove
 * it from the list and copy the rest of the buffer to the request.
 * The request is finished by calling request_end()
 */
static ssize_t fuse_dev_do_write(struct fuse_conn *fc, struct iov_iter *iter)
{
	int err;
	struct fuse_req *req;
	struct fuse_out_header oh;
	size_t len;
	size_t nbytes = iter->count;

	if (iter->count < sizeof(struct fuse_out_header))
		return -EINVAL;

	len = sizeof(oh);
	if (copy_from_iter(&oh, len, iter) != len) {
		printk(KERN_ERR "%s: can't copy header\n", __func__);
		return -EFAULT;
	}

	if (oh.len != nbytes)
		return -EINVAL;

	/*
	 * Zero oh.unique indicates unsolicited notification message
	 * and error contains notification code.
	 */
	if (!oh.unique) {
		err = fuse_notify(fc, oh.error, nbytes - sizeof(oh), iter);
		return err ? err : nbytes;
	}

	if (oh.error <= -1000 || oh.error > 0)
		return -EINVAL;

	err = -ENOENT;

	req = request_find(fc, oh.unique);
	if (!req) {
		printk(KERN_ERR "%s: request %lld not found\n", __func__, oh.unique);
		return -ENOENT;
	}

	req->out.h = oh;

	if (req->in.h.opcode == PXD_READ && iter->count > 0) {
		struct request *breq = req->rq;
#ifdef HAVE_BVEC_ITER
		struct bio_vec bvec;
#else
		struct bio_vec *bvec = NULL;
#endif
		struct req_iterator breq_iter;
		if (breq->nr_phys_segments) {
			int i = 0;
			rq_for_each_segment(bvec, breq, breq_iter) {
				len = BVEC(bvec).bv_len;
				if (copy_page_from_iter(BVEC(bvec).bv_page,
							BVEC(bvec).bv_offset,
							len, iter) != len) {
					printk(KERN_ERR "%s: copy page %d of %d error\n",
					       __func__, i, breq->nr_phys_segments);
					return -EFAULT;
				}
				i++;
			}
		}
	}
	request_end(fc, req);
	return nbytes;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(4,0,0)
static ssize_t fuse_dev_write(struct kiocb *iocb, const struct iovec *iov,
			      unsigned long nr_segs, loff_t pos)
{
	struct fuse_conn *fc = fuse_get_conn(iocb->ki_filp);
	struct iov_iter iter;
	if (!fc)
		return -EPERM;

	iov_iter_init(&iter, WRITE, iov, nr_segs, iov_length(iov, nr_segs));

	return fuse_dev_do_write(fc, &iter);
}
#else
static ssize_t fuse_dev_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	struct fuse_conn *fc = fuse_get_conn(iocb->ki_filp);
	if (!fc)
		return -EPERM;

	return fuse_dev_do_write(fc, from);
}
#endif

static ssize_t fuse_dev_splice_write(struct pipe_inode_info *pipe,
				     struct file *out, loff_t *ppos,
				     size_t len, unsigned int flags)
{
	return -EINVAL;
}

static unsigned fuse_dev_poll(struct file *file, poll_table *wait)
{
	unsigned mask = POLLOUT | POLLWRNORM;
	struct fuse_conn *fc = fuse_get_conn(file);
	if (!fc)
		return POLLERR;

	poll_wait(file, &fc->waitq, wait);

	spin_lock(&fc->lock);
	if (!fc->connected)
		mask = POLLERR;
	else if (request_pending(fc))
		mask |= POLLIN | POLLRDNORM;
	spin_unlock(&fc->lock);

	return mask;
}

static void end_queued_requests(struct fuse_conn *fc)
__releases(fc->lock)
__acquires(fc->lock)
{
	int i;
	for (i = 0; i < FUSE_REQUEST_QUEUE_SIZE; ++i) {
		struct fuse_req *req = fc->request_map[i];
		if (req != NULL) {
			req->out.h.error = -ECONNABORTED;
			request_end(fc, req);
		}
	}
}

static void fuse_conn_free_allocs(struct fuse_conn *fc)
{
	if (fc->per_cpu_ids)
		free_percpu(fc->per_cpu_ids);
	if (fc->free_ids)
		kfree(fc->free_ids);
	if (fc->request_map)
		kfree(fc->request_map);
	if (fc->queue.w.requests)
		vfree(fc->queue.w.requests);
}

static int fuse_req_queue_init(struct fuse_req_queue *queue)
{
	size_t alloc_size = FUSE_REQUEST_QUEUE_SIZE * sizeof(queue->w.requests[0]);
	queue->w.requests = vmalloc(alloc_size);
	if (queue->w.requests == NULL)
		return -ENOMEM;
	memset(queue->w.requests, 0, alloc_size);

	queue->w.sequence = 1;
	queue->w.read = 0;
	queue->w.write = 0;
	spin_lock_init(&queue->w.lock);

	queue->r.requests = queue->w.requests;
	queue->r.write = 0;
	queue->r.read = 0;

	return 0;
}

int fuse_conn_init(struct fuse_conn *fc)
{
	int i, rc;
	int cpu;

	memset(fc, 0, sizeof(*fc));
	spin_lock_init(&fc->lock);
	atomic_set(&fc->count, 1);
	init_waitqueue_head(&fc->waitq);
	INIT_LIST_HEAD(&fc->entry);
	fc->request_map = kmalloc(FUSE_MAX_REQUEST_IDS * sizeof(struct fuse_req*),
		GFP_KERNEL);

	rc = -ENOMEM;
	if (!fc->request_map) {
		printk(KERN_ERR "failed to allocate request map");
		goto err_out;
	}
	memset(fc->request_map, 0,
		FUSE_MAX_REQUEST_IDS * sizeof(struct fuse_req*));

	fc->free_ids = kmalloc(FUSE_MAX_REQUEST_IDS * sizeof(u64), GFP_KERNEL);
	if (!fc->free_ids) {
		printk(KERN_ERR "failed to allocate free requests");
		goto err_out;
	}
	for (i = 0; i < FUSE_MAX_REQUEST_IDS; ++i) {
		fc->free_ids[i] = FUSE_MAX_REQUEST_IDS - i - 1;
	}
	fc->num_free_ids = FUSE_MAX_REQUEST_IDS;

	fc->per_cpu_ids = alloc_percpu(struct fuse_per_cpu_ids);
	if (!fc->per_cpu_ids) {
		printk(KERN_ERR "failed to allocate per cpu ids");
		goto err_out;
	}

	/* start with nothing allocated to cpus */
	for_each_possible_cpu(cpu) {
		struct fuse_per_cpu_ids *my_ids = per_cpu_ptr(fc->per_cpu_ids, cpu);
		memset(my_ids, 0, sizeof(*my_ids));
	}

	fc->reqctr = 0;

	rc = fuse_req_queue_init(&fc->queue);
	if (rc != 0)
		goto err_out;

	return 0;
err_out:
	fuse_conn_free_allocs(fc);
	return rc;
}

void fuse_conn_put(struct fuse_conn *fc)
{
	if (atomic_dec_and_test(&fc->count)) {
		fuse_conn_free_allocs(fc);
		fc->release(fc);

	}
}

struct fuse_conn *fuse_conn_get(struct fuse_conn *fc)
{
	atomic_inc(&fc->count);
	return fc;
}

/*
 * Abort all requests.
 *
 * Emergency exit in case of a malicious or accidental deadlock, or
 * just a hung filesystem.
 *
 * The same effect is usually achievable through killing the
 * filesystem daemon and all users of the filesystem.  The exception
 * is the combination of an asynchronous request and the tricky
 * deadlock (see Documentation/filesystems/fuse.txt).
 *
 * During the aborting, progression of requests from the pending and
 * processing lists onto the io list, and progression of new requests
 * onto the pending list is prevented by req->connected being false.
 *
 * Progression of requests under I/O to the processing list is
 * prevented by the req->aborted flag being true for these requests.
 * For this reason requests on the io list must be aborted first.
 */
void fuse_abort_conn(struct fuse_conn *fc)
{
	spin_lock(&fc->lock);
	if (fc->connected) {
		fc->connected = 0;
		end_queued_requests(fc);
		wake_up_all(&fc->waitq);
		kill_fasync(&fc->fasync, SIGIO, POLL_IN);
	}
	spin_unlock(&fc->lock);
}

int fuse_dev_release(struct inode *inode, struct file *file)
{
	struct fuse_conn *fc = fuse_get_conn(file);
	if (fc) {
		spin_lock(&fc->lock);
		fc->connected = 0;
		end_queued_requests(fc);
		spin_unlock(&fc->lock);
		fuse_conn_put(fc);
	}

	return 0;
}

static int compare_reqs(const void *lhs, const void *rhs)
{
	struct fuse_req *lhs_req = *(struct fuse_req**)lhs;
	struct fuse_req *rhs_req = *(struct fuse_req**)rhs;



	if (lhs_req->sequence < rhs_req->sequence)
		return -1;
	if (lhs_req->sequence > rhs_req->sequence)
		return 1;
	return 0;
}

/* Request map contains all pending requests. Add them back to the queue sorted by
 * original request order. This function is called when the reader is inactive
 * and reader part can be safely modified.
 */
int fuse_restart_requests(struct fuse_conn *fc)
{
	int i;
	struct fuse_req **resend_reqs;
	u32 read = fc->queue.r.read;	/* ok to access read part since user space is
 					* inactive */
	u32 write;
	u64 sequence;
	int resend_count = 0;

	/*
	 * Receive function may be adding new requests while scan is in progress.
	 * Find the sequence of the first request unread by user space. If there are no
	 * pending requests, use the next request sequence.
	 */
	spin_lock(&fc->queue.w.lock);
	sequence = fc->queue.w.sequence;
	write = fc->queue.w.write;
	if (read != write)
		sequence = fc->queue.w.requests[read]->sequence;
	spin_unlock(&fc->queue.w.lock);

	printk(KERN_INFO "read %d write %d sequence %lld", read, write, sequence);

	resend_reqs = vmalloc(sizeof(struct fuse_req *) * FUSE_REQUEST_QUEUE_SIZE);
	if (resend_reqs == NULL)
		return -ENOMEM;

	/* Add all pending requests with lower sequence to resend list */
	for (i = 0; i < FUSE_REQUEST_QUEUE_SIZE; ++i) {
		struct fuse_req *req = fc->request_map[i];
		if (req == NULL)
			continue;
		if (req->sequence < sequence)
			resend_reqs[resend_count++] = req;
	}

	sort(resend_reqs, resend_count, sizeof(struct fuse_req*), &compare_reqs, NULL);

	/* Put requests back into the queue*/
	for (i = resend_count; i != 0; --i) {
		read = (read - 1) & (FUSE_REQUEST_QUEUE_SIZE - 1);
		fc->queue.w.requests[read] = resend_reqs[i - 1];
	}

	spin_lock(&fc->queue.w.lock);
	fc->queue.w.read = read;
	/* update the reader part */
	fc->queue.r.read = read;
	spin_unlock(&fc->queue.w.lock);

	spin_lock(&fc->lock);
	fuse_conn_wakeup(fc);
	spin_unlock(&fc->lock);

	vfree(resend_reqs);

	return 0;
}

static int fuse_dev_fasync(int fd, struct file *file, int on)
{
	struct fuse_conn *fc = fuse_get_conn(file);
	if (!fc)
		return -EPERM;

	/* No locking - fasync_helper does its own locking */
	return fasync_helper(fd, file, on, &fc->fasync);
}


#if LINUX_VERSION_CODE < KERNEL_VERSION(4,0,0)
const struct file_operations fuse_dev_operations = {
	.owner		= THIS_MODULE,
	.llseek		= no_llseek,
	.read		= do_sync_read,
	.aio_read	= fuse_dev_read,
	.splice_read	= fuse_dev_splice_read,
	.write		= do_sync_write,
	.aio_write	= fuse_dev_write,
	.splice_write	= fuse_dev_splice_write,
	.poll		= fuse_dev_poll,
	.release	= fuse_dev_release,
	.fasync		= fuse_dev_fasync,
};
#else
const struct file_operations fuse_dev_operations = {
	.owner		= THIS_MODULE,
	.llseek		= no_llseek,
	.read_iter	= fuse_dev_read_iter,
	.splice_read	= fuse_dev_splice_read,
	.write_iter	= fuse_dev_write_iter,
	.splice_write	= fuse_dev_splice_write,
	.poll		= fuse_dev_poll,
	.release	= fuse_dev_release,
	.fasync		= fuse_dev_fasync,
};
#endif

int fuse_dev_init(void)
{
	int err = -ENOMEM;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,16,0)
	fuse_req_cachep = kmem_cache_create_usercopy("pxd_fuse_request",
					    sizeof(struct fuse_req),
					    0, 0, 0, sizeof(struct fuse_req), NULL);
#else
	fuse_req_cachep = kmem_cache_create("pxd_fuse_request",
					    sizeof(struct fuse_req),
					    0, 0, NULL);
#endif
	if (!fuse_req_cachep)
		goto out;

	return 0;

 out:
	return err;
}

void fuse_dev_cleanup(void)
{
	kmem_cache_destroy(fuse_req_cachep);
}
