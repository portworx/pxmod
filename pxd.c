#include "fuse.h"
#include <linux/module.h>
#include <linux/module.h>
#include <linux/blkdev.h>
#include <linux/sysfs.h>
#include <linux/crc32.h>
#include <linux/ctype.h>
#include <linux/sched.h>
#include "fuse_i.h"
#include "pxd.h"
#include <linux/uio.h>
#include <linux/bio.h>
#include <linux/pid_namespace.h>

#if defined(RHEL_RELEASE_CODE) && defined(RHEL_RELEASE_VERSION) && defined(__EL8__)
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,14,0) && RHEL_RELEASE_CODE >= RHEL_RELEASE_VERSION(9,4)
#define __RHEL_GT_94__
#endif
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,3,0) && !defined(part_stat_lock)
#include <linux/part_stat.h>
#endif

#ifndef READ_ONCE
#define READ_ONCE(x) x
#endif

#ifndef WRITE_ONCE
#define WRITE_ONCE(x, val) x = val
#endif

#define CREATE_TRACE_POINTS
#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .
#define TRACE_INCLUDE_FILE pxd_trace
#include "pxd_trace.h"
#undef CREATE_TRACE_POINTS

#include "pxd_compat.h"
#include "pxd_core.h"

#ifdef __PX_BLKMQ__
#include <linux/blk-mq.h>
#endif

/** enables time tracing */
//#define GD_TIME_LOG
#ifdef GD_TIME_LOG
#define KTIME_GET_TS(t) ktime_get_ts((t))
#else
#define KTIME_GET_TS(t)
#endif

#define PXD_TIMER_SECS_MIN 30
#define PXD_TIMER_SECS_DEFAULT 600
#define PXD_TIMER_SECS_MAX (U32_MAX)

#if defined(PF_LESS_THROTTLE)
#define PF_IO_FLUSHER (PF_MEMALLOC_NOIO | PF_LESS_THROTTLE)
#elif defined(PF_LOCAL_THROTTLE)
#define PF_IO_FLUSHER (PF_MEMALLOC_NOIO | PF_LOCAL_THROTTLE)
#endif
#define IS_SET_IO_FLUSHER_FLAG(flg) ((flg & PF_IO_FLUSHER) == PF_IO_FLUSHER)
#define IS_SET_IO_FLUSHER(task) (IS_SET_IO_FLUSHER_FLAG((task->flags)))
#define SET_IO_FLUSHER(task) (task->flags |= PF_IO_FLUSHER)
#define CLEAR_IO_FLUSHER(task) (task->flags &= ~(PF_IO_FLUSHER))

#define TOSTRING_(x) #x
#define VERTOSTR(x) TOSTRING_(x)

extern const char *gitversion;
static dev_t pxd_major;
static DEFINE_IDA(pxd_minor_ida);

struct pxd_context *pxd_contexts;
uint32_t pxd_num_contexts = PXD_NUM_CONTEXTS;
uint32_t pxd_num_contexts_exported = PXD_NUM_CONTEXT_EXPORTED;
uint32_t pxd_timeout_secs = PXD_TIMER_SECS_DEFAULT;
uint32_t pxd_detect_zero_writes = 0;
uint32_t pxd_num_fpthreads = DEFAULT_PXFP_WORKERS_PER_NODE;

module_param(pxd_num_contexts_exported, uint, 0644);
module_param(pxd_num_contexts, uint, 0644);
module_param(pxd_detect_zero_writes, uint, 0644);
module_param(pxd_num_fpthreads, uint, 0644);

static void pxd_abort_context(struct work_struct *work);
static int pxd_nodewipe_cleanup(struct pxd_context *ctx);
static int pxd_bus_add_dev(struct pxd_device *pxd_dev);

struct pxd_context* find_context(unsigned ctx)
{
	if (ctx >= pxd_num_contexts) {
		return NULL;
	}

	return &pxd_contexts[ctx];
}


#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,5,0) || defined(__RHEL_GT_94__)  || defined(__SUSE_GTE_SP6__) || defined(__SLE_MICRO_GTE_6_0__)
static int pxd_open(struct gendisk *bdev, blk_mode_t mode)
#else
static int pxd_open(struct block_device *bdev, fmode_t mode)
#endif
{
	struct pxd_device *pxd_dev;
	int err = 0;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,5,0) || defined(__RHEL_GT_94__)  || defined(__SUSE_GTE_SP6__) || defined(__SLE_MICRO_GTE_6_0__)
	pxd_dev = bdev->private_data;
#else
	pxd_dev = bdev->bd_disk->private_data;
#endif
	if (!pxd_dev) {
		return -ENXIO;
	}

	spin_lock(&pxd_dev->lock);
	if (!pxd_dev->connected) {
		err = -ENXIO;
	} else {
		if (pxd_dev->removing)
			err = -EBUSY;
		else
			pxd_dev->open_count++;

		if (!err)
			(void)get_device(&pxd_dev->dev);
	}
	spin_unlock(&pxd_dev->lock);

	trace_pxd_open(pxd_dev->dev_id, pxd_dev->major, pxd_dev->minor, mode, err);
	return err;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,5,0) || defined(__RHEL_GT_94__) || defined(__SUSE_GTE_SP6__) || defined(__SLE_MICRO_GTE_6_0__)
static void pxd_release(struct gendisk *disk)
#else
static void pxd_release(struct gendisk *disk, fmode_t mode)
#endif
{
	struct pxd_device *pxd_dev;

	pxd_dev = disk->private_data;
	if (!pxd_dev) {
		printk(KERN_WARNING"pxd empty device context\n");
		return;
	}

	spin_lock(&pxd_dev->lock);
	BUG_ON(pxd_dev->magic != PXD_DEV_MAGIC);
	pxd_dev->open_count--;
	spin_unlock(&pxd_dev->lock);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,5,0) || defined(__RHEL_GT_94__) || defined(__SUSE_GTE_SP6__) || defined(__SLE_MICRO_GTE_6_0__)
	trace_pxd_release(pxd_dev->dev_id, pxd_dev->major, pxd_dev->minor);
#else
	trace_pxd_release(pxd_dev->dev_id, pxd_dev->major, pxd_dev->minor, mode);
#endif
	put_device(&pxd_dev->dev);
}

static long pxd_ioctl_dump_fc_info(void)
{
	int i;
	struct pxd_context *ctx;

	for (i = 0; i < pxd_num_contexts; ++i) {
		ctx = &pxd_contexts[i];
		if (ctx->num_devices == 0) {
			continue;
		}
		printk(KERN_INFO "%s: pxd_ctx: %s ndevices: %lu",
			__func__, ctx->name, ctx->num_devices);
		printk(KERN_INFO "\tFC: connected: %d", READ_ONCE(ctx->fc.connected));
	}
	return 0;
}

static long pxd_ioctl_get_version(void __user *argp)
{
	char ver_data[64];
	int ver_len = 0;

	if (argp) {
		ver_len = strlen(gitversion) + 1; // account for null
		if (sizeof(ver_data) < ver_len) {
			ver_len = sizeof(ver_data);
		}
		strncpy(ver_data, gitversion, ver_len);
		ver_data[ver_len-1]='\0';
		if (copy_to_user(argp +
				 offsetof(struct pxd_ioctl_version_args, piv_len),
			&ver_len, sizeof(ver_len))) {
			return -EFAULT;
		}
		if (copy_to_user(argp +
				 offsetof(struct pxd_ioctl_version_args, piv_data),
			ver_data, ver_len)) {
			return -EFAULT;
		}
	}

	return 0;
}

static long pxd_ioctl_init(struct file *file, void __user *argp)
{
	struct pxd_context *ctx = container_of(file->f_op, struct pxd_context, fops);
	struct iov_iter iter;
	struct iovec iov = {argp, sizeof(struct pxd_ioctl_init_args)};
	int direction = READ;

	iov_iter_init(&iter, direction, &iov, 1, sizeof(struct pxd_ioctl_init_args));

	return pxd_read_init(&ctx->fc, &iter);
}

static long pxd_ioctl_resize(struct file *file, void __user *argp)
{
	struct pxd_context *ctx = NULL;
	struct pxd_update_size update_args;
	long ret = 0;

	if (copy_from_user(&update_args, argp, sizeof(update_args))) {
		return -EFAULT;
	}

	if (update_args.context_id >= pxd_num_contexts_exported) {
		printk("%s : invalid context: %d\n", __func__, update_args.context_id);
		return -EFAULT;
	}

	ctx =  &pxd_contexts[update_args.context_id];
	if (!ctx || ctx->id >= pxd_num_contexts_exported) {
		return -EFAULT;
	}

	ret = pxd_ioc_update_size(&ctx->fc, &update_args);
	return ret;
}

static long pxd_ioctl_fp_cleanup(struct file *file, void __user *argp)
{
	struct pxd_context *ctx = NULL;
	struct pxd_fastpath_out cleanup_args;
	long ret = 0;
	struct pxd_device *pxd_dev;

	if (copy_from_user(&cleanup_args, argp, sizeof(cleanup_args))) {
		return -EFAULT;
	}

	if (cleanup_args.context_id >= pxd_num_contexts_exported) {
		printk("%s : invalid context: %d\n", __func__, cleanup_args.context_id);
		return -EFAULT;
	}

	ctx =  &pxd_contexts[cleanup_args.context_id];
	if (!ctx || ctx->id >= pxd_num_contexts_exported) {
		return -EFAULT;
	}

	pxd_dev = find_pxd_device(ctx, cleanup_args.dev_id);
	if (pxd_dev != NULL) {
		(void)get_device(&pxd_dev->dev);
		ret = pxd_fastpath_vol_cleanup(pxd_dev);
		put_device(&pxd_dev->dev);
	}

	return ret;
}

static void print_io_flusher_state(unsigned int new_flags,
				   pid_t pid, pid_t ppid, char *comm)
{
	if (IS_SET_IO_FLUSHER_FLAG(new_flags)) {
		printk("Process %s pid %d parent pid %d IO_FLUSHER is set\n",
			       comm, pid, ppid);
	} else {
		printk("Process %s pid %d parent pid %d IO_FLUSHER not set\n", comm, pid,
				ppid);
	}
}

static int is_io_flusher_supported(void)
{
  	#if LINUX_VERSION_CODE >= KERNEL_VERSION(3,10,0) && defined(PF_MEMALLOC_NOIO)
                #if defined(PF_LESS_THROTTLE) || defined(PF_LOCAL_THROTTLE)
			return 1;
		#else
			return 0;
		#endif
	#else
		return 0;
	#endif
}

static long pxd_ioflusher_state(void __user *argp)
{
	struct pxd_ioctl_io_flusher_args io_flusher_args;
	struct task_struct *task = NULL;
	struct pid_namespace *ns = NULL;
	pid_t ppid = -1;
	pid_t pid = -1;
	char *comm = NULL;
	unsigned int old_flags = 0;
	unsigned int new_flags = 0;
	char command[TASK_COMM_LEN] = {0};
	int is_io_flusher_set = 0;

	if (!is_io_flusher_supported()) {
		return -EOPNOTSUPP;
	}

	if (argp == NULL) {
		return -EINVAL;
	}

	if (copy_from_user(&io_flusher_args, argp, sizeof(io_flusher_args))) {
		return -EFAULT;
	}
	rcu_read_lock();
	task = current;
	ns = task_active_pid_ns(task);
	if (io_flusher_args.pid == 0) {
		get_task_struct(task);
	} else {
		struct task_struct *temp_task;
		struct pid *pid = NULL;
		pid = find_pid_ns(io_flusher_args.pid, ns);
		if (pid == NULL) {
			rcu_read_unlock();
			printk("Input pid %d does not exist", io_flusher_args.pid);
			return -ENOENT;
		}
		temp_task = pid_task(pid, PIDTYPE_PID);
		if (temp_task == NULL) {
			rcu_read_unlock();
			printk("Input pid %d does not exist", io_flusher_args.pid);
			return -ENOENT;
		}
		task = temp_task;
		get_task_struct(task);
	}
	rcu_read_unlock();
	ppid = task_pgrp_nr(task);
	pid = task_pid_nr(task);
	comm = get_task_comm(command, task);
	old_flags = task->flags;
	switch (io_flusher_args.io_flusher_action) {
	case PXD_IO_FLUSHER_GET:
		break;
	case PXD_IO_FLUSHER_SET:
		SET_IO_FLUSHER(task);
		break;
	case PXD_IO_FLUSHER_CLEAR:
		CLEAR_IO_FLUSHER(task);
		break;
	}
	new_flags = task->flags;
	put_task_struct(task);

	is_io_flusher_set = IS_SET_IO_FLUSHER_FLAG(new_flags);

	print_io_flusher_state(new_flags, pid, ppid, comm);

	if (copy_to_user(argp + offsetof(struct pxd_ioctl_io_flusher_args, is_io_flusher_set),
			&is_io_flusher_set, sizeof(is_io_flusher_set))) {
		return -EFAULT;
	}

	return 0;
}

static long pxd_control_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	switch (cmd) {
	case PXD_IOC_DUMP_FC_INFO:
		return pxd_ioctl_dump_fc_info();
	case PXD_IOC_GET_VERSION:
		return pxd_ioctl_get_version((void __user *)arg);
	case PXD_IOC_INIT:
		return pxd_ioctl_init(file, (void __user *)arg);
	case PXD_IOC_RESIZE:
		return pxd_ioctl_resize(file, (void __user *)arg);
	case PXD_IOC_FPCLEANUP:
		return pxd_ioctl_fp_cleanup(file, (void __user *)arg);
	case PXD_IOC_IO_FLUSHER:
		return pxd_ioflusher_state((void __user *)arg);
	default:
		return -ENOTTY;
	}
}

static const struct block_device_operations pxd_bd_ops = {
	.owner			= THIS_MODULE,
	.open			= pxd_open,
	.release		= pxd_release,
};

#if defined(__PXD_BIO_MAKEREQ__) && LINUX_VERSION_CODE >= KERNEL_VERSION(5,9,0)
static const struct block_device_operations pxd_bd_fpops = {
    .owner          = THIS_MODULE,
    .open           = pxd_open,
    .release        = pxd_release,
    .submit_bio     = pxd_bio_make_request_entryfn,
};
#define get_bd_fpops() (&pxd_bd_fpops)
#else
#define get_bd_fpops() (&pxd_bd_ops)
#endif


static bool __pxd_device_qfull(struct pxd_device *pxd_dev)
{
	int ncount = PXD_ACTIVE(pxd_dev);

	// does not care about async or sync request.
	if (ncount > pxd_dev->qdepth) {
		if (atomic_cmpxchg(&pxd_dev->congested, 0, 1) == 0) {
			pxd_dev->nr_congestion_on++;
		}
		return 1;
	}
	if (atomic_cmpxchg(&pxd_dev->congested, 1, 0) == 1) {
		pxd_dev->nr_congestion_off++;
	}

	return 0;
}

// congestion callback from kernel writeback module
int pxd_device_congested(void *data, int bits)
{
	struct pxd_device *pxd_dev = data;

	// notify congested if device is suspended as well.
	// modified under lock, read outside lock.
	if (atomic_read(&pxd_dev->fp.suspend)) {
		return 1;
	}

	return __pxd_device_qfull(pxd_dev);
}

void pxd_check_q_congested(struct pxd_device *pxd_dev)
{
	if (pxd_device_congested(pxd_dev, 0)) {
		wait_event_interruptible(pxd_dev->suspend_wq,
			!pxd_device_congested(pxd_dev, 0));
	}
}

void pxd_check_q_decongested(struct pxd_device *pxd_dev)
{
	if (!pxd_device_congested(pxd_dev, 0)) {
		wake_up(&pxd_dev->suspend_wq);
	}
}

static void pxd_request_complete(struct fuse_conn *fc, struct fuse_req *req, int status)
{
	atomic_dec(&req->pxd_dev->ncount);
	pxd_check_q_decongested(req->pxd_dev);
	pxd_printk("%s: receive reply to %px(%lld) at %lld err %d\n",
			__func__, req, req->in.h.unique,
			req->pxd_rdwr_in.offset, status);
}

#ifdef __PXD_BIO_MAKEREQ__
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,11,0)
static void pxd_update_stats(struct fuse_req *req, int sgrp, unsigned int count)
#else
static void pxd_update_stats(struct fuse_req *req, int rw, unsigned int count)
#endif
{
		struct pxd_device *pxd_dev = req->queue->queuedata;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,11,0) || defined(__EL8__)
{
		struct block_device *p = pxd_dev->disk->part0;
		if (!p) return;


		part_stat_lock();
		part_stat_add(p, sectors[sgrp], count);
		part_stat_inc(p, ios[sgrp]);
}
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(5,0,0) || defined(__EL8__)
		part_stat_lock();
		part_stat_inc(&pxd_dev->disk->part0, ios[rw]);
		part_stat_add(&pxd_dev->disk->part0, sectors[rw], count);
#else
		int cpu = part_stat_lock();
		part_stat_inc(cpu, &pxd_dev->disk->part0, ios[rw]);
		part_stat_add(cpu, &pxd_dev->disk->part0, sectors[rw], count);
#endif
		part_stat_unlock();
}

static bool pxd_process_read_reply(struct fuse_conn *fc, struct fuse_req *req,
		int status)
{
	trace_pxd_reply(req->in.h.unique, 0u);
	pxd_update_stats(req, 0, BIO_SIZE(req->bio) / SECTOR_SIZE);
	BIO_ENDIO(req->bio, status);
	pxd_request_complete(fc, req, status);

	return true;
}

static bool pxd_process_write_reply(struct fuse_conn *fc, struct fuse_req *req,
		int status)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,8,0) || defined(REQ_PREFLUSH)
	trace_pxd_reply(req->in.h.unique, REQ_OP_WRITE);
#else
	trace_pxd_reply(req->in.h.unique, REQ_WRITE);
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,11,0)
{
        const struct bio* bio = req->bio;
        int statgrp = STAT_WRITE;
        size_t sz = BIO_SIZE(bio) / SECTOR_SIZE;

        if (!bio) statgrp = STAT_FLUSH;
        else if (!op_is_write(bio->bi_opf)) statgrp = STAT_READ;
        else if (op_is_flush(bio->bi_opf) && (sz == 0)) statgrp = STAT_FLUSH;
        else statgrp = STAT_WRITE;

        pxd_update_stats(req, statgrp, sz);
}
#else
	pxd_update_stats(req, 1, BIO_SIZE(req->bio) / SECTOR_SIZE);
#endif
	BIO_ENDIO(req->bio, status);
	pxd_request_complete(fc, req, status);

	return true;
}

#else

/* only used by the USE_REQUESTQ_MODEL definition */
static bool pxd_process_read_reply_q(struct fuse_conn *fc, struct fuse_req *req,
		int status)
{
	pxd_request_complete(fc, req, status);
#ifndef __PX_BLKMQ__
	blk_end_request(req->rq, status, blk_rq_bytes(req->rq));
	return true;
#else
	blk_mq_end_request(req->rq, errno_to_blk_status(status));
	return false;
#endif
}

/* only used by the USE_REQUESTQ_MODEL definition */
static bool pxd_process_write_reply_q(struct fuse_conn *fc, struct fuse_req *req,
		int status)
{

	pxd_request_complete(fc, req, status);
#ifndef __PX_BLKMQ__
	blk_end_request(req->rq, status, blk_rq_bytes(req->rq));
	return true;
#else
	blk_mq_end_request(req->rq, errno_to_blk_status(status));
	return false;
#endif
}
#endif

static struct fuse_req *pxd_fuse_req(struct pxd_device *pxd_dev)
{
	int eintr = 0;
	struct fuse_req *req = NULL;
	struct fuse_conn *fc = &pxd_dev->ctx->fc;
	int status;

	while (req == NULL) {
		req = fuse_get_req_for_background(fc);
		if (IS_ERR(req) && PTR_ERR(req) == -EINTR) {
			req = NULL;
			++eintr;
		}
	}
	if (eintr > 0) {
		printk_ratelimited(KERN_INFO "%s: alloc EINTR retries %d",
			 __func__, eintr);
	}
	status = IS_ERR(req) ? PTR_ERR(req) : 0;
	if (status != 0) {
		printk_ratelimited(KERN_ERR "%s: request alloc failed: %d",
			 __func__, status);
	}

	return req;
}

static void pxd_req_misc(struct fuse_req *req, uint32_t size, uint64_t off,
			uint32_t minor, uint32_t flags)
{
	req->in.numargs = 1;
	req->in.args[0].size = sizeof(struct pxd_rdwr_in);
	req->in.args[0].value = &req->pxd_rdwr_in;
	req->in.h.pid = current->pid;
	req->pxd_rdwr_in.dev_minor = minor;
	req->pxd_rdwr_in.offset = off;
	req->pxd_rdwr_in.size = size;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,8,0) || defined(REQ_PREFLUSH)
	req->pxd_rdwr_in.flags =
		((flags & REQ_FUA) ? PXD_FLAGS_FLUSH : 0) |
		((flags & REQ_PREFLUSH) ? PXD_FLAGS_FLUSH : 0) |
		((flags & REQ_META) ? PXD_FLAGS_META : 0);
#else
	req->pxd_rdwr_in.flags = ((flags & REQ_FLUSH) ? PXD_FLAGS_FLUSH : 0) |
					  ((flags & REQ_FUA) ? PXD_FLAGS_FUA : 0) |
					  ((flags & REQ_META) ? PXD_FLAGS_META : 0);
#endif
}

/*
 * when block device is registered in non blk mq mode, device limits are not
 * honoured. Handle it appropriately.
 */
#ifdef __PXD_BIO_MAKEREQ__
static
int pxd_handle_device_limits(struct fuse_req *req, uint32_t *size, uint64_t *off,
		unsigned int op)
{
	struct request_queue *q = req->pxd_dev->disk->queue;
	sector_t max_sectors, rq_sectors;

	if (!fastpath_enabled(req->pxd_dev)) {
		return 0;
	}

	rq_sectors = *size >> SECTOR_SHIFT;
	BUG_ON(rq_sectors != bio_sectors(req->bio));

	max_sectors = blk_queue_get_max_sectors(q, op);
	if (!max_sectors) {
		return -EOPNOTSUPP;
	}

	while (rq_sectors > max_sectors) {
		struct bio *b;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,18,0) || \
    (LINUX_VERSION_CODE >= KERNEL_VERSION(4,12,0) && \
     defined(bvec_iter_sectors))
		b = bio_split(req->bio, max_sectors, GFP_NOIO, &fs_bio_set);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(3,14,0)
		b = bio_split(req->bio, max_sectors, GFP_NOIO, fs_bio_set);
#else
		// This issue has so far been seen only with 4.20 and 5.x kernels
		// bio split signature way too different to be handled.
		printk_ratelimited(KERN_ERR"device %llu IO queue limits (rq/max %lu/%lu sectors) exceeded\n",
			req->pxd_dev->dev_id, rq_sectors, max_sectors);
		return -EIO;
#endif
		if (!b) {
			return -ENOMEM;
		}

		bio_chain(b, req->bio);
#if LINUX_VERSION_CODE < KERNEL_VERSION(5,9,0)
		generic_make_request(b);
#else
		submit_bio_noacct(b);
#endif
		rq_sectors -= max_sectors;
		*off += (max_sectors << SECTOR_SHIFT);
	}

	*size = rq_sectors << SECTOR_SHIFT;
	return 0;
}
#else
static inline
int pxd_handle_device_limits(struct fuse_req *req, uint32_t *size, uint64_t *off,
		unsigned int op)
{
	return 0;
}
#endif

static int pxd_read_request(struct fuse_req *req, uint32_t size, uint64_t off,
			uint32_t minor, uint32_t flags)
{
	int rc;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,8,0) || defined(REQ_PREFLUSH)
    rc = pxd_handle_device_limits(req, &size, &off, REQ_OP_READ);
#else
    rc = pxd_handle_device_limits(req, &size, &off, 0);
#endif
	if (rc) {
		return rc;
	}

	req->in.h.opcode = PXD_READ;
#ifdef __PXD_BIO_MAKEREQ__
	req->end = pxd_process_read_reply;
#else
	req->end = pxd_process_read_reply_q;
#endif

	pxd_req_misc(req, size, off, minor, flags);
	return 0;
}

static int pxd_write_request(struct fuse_req *req, uint32_t size, uint64_t off,
			uint32_t minor, uint32_t flags)
{
	int rc;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,8,0) || defined(REQ_PREFLUSH)
    rc = pxd_handle_device_limits(req, &size, &off, REQ_OP_WRITE);
#else
    rc = pxd_handle_device_limits(req, &size, &off, REQ_WRITE);
#endif
	if (rc) {
		return rc;
	}

	req->in.h.opcode = PXD_WRITE;
#ifdef __PXD_BIO_MAKEREQ__
	req->end = pxd_process_write_reply;
#else
	req->end = pxd_process_write_reply_q;
#endif

	pxd_req_misc(req, size, off, minor, flags);

	if (pxd_detect_zero_writes && req->pxd_rdwr_in.size != 0)
		fuse_convert_zero_writes(req);

	return 0;
}

static int pxd_discard_request(struct fuse_req *req, uint32_t size, uint64_t off,
			uint32_t minor, uint32_t flags)
{
	int rc;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,8,0) || defined(REQ_PREFLUSH)
	rc = pxd_handle_device_limits(req, &size, &off, REQ_OP_DISCARD);
#else
	rc = pxd_handle_device_limits(req, &size, &off, REQ_DISCARD);
#endif
	if (rc) {
		return rc;
	}

	req->in.h.opcode = PXD_DISCARD;
#ifdef __PXD_BIO_MAKEREQ__
	req->end = pxd_process_write_reply;
#else
	req->end = pxd_process_write_reply_q;
#endif

	pxd_req_misc(req, size, off, minor, flags);
	return 0;
}

static int pxd_write_same_request(struct fuse_req *req, uint32_t size, uint64_t off,
			uint32_t minor, uint32_t flags)
{
	int rc;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,18,0) || (LINUX_VERSION_CODE >= KERNEL_VERSION(5,14,0) && (defined(__EL8__) || defined(__SUSE_EQ_SP5__)))
	rc = pxd_handle_device_limits(req, &size, &off, REQ_OP_WRITE_ZEROES);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(4,8,0) || defined(REQ_PREFLUSH)
	rc = pxd_handle_device_limits(req, &size, &off, REQ_OP_WRITE_SAME);
#else
	rc = pxd_handle_device_limits(req, &size, &off, REQ_WRITE_SAME);
#endif
	if (rc) {
		return rc;
	}

	req->in.h.opcode = PXD_WRITE_SAME;
#ifdef __PXD_BIO_MAKEREQ__
	req->end = pxd_process_write_reply;
#else
	req->end = pxd_process_write_reply_q;
#endif

	pxd_req_misc(req, size, off, minor, flags);
	return 0;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,8,0) || defined(REQ_PREFLUSH)
static int pxd_request(struct fuse_req *req, uint32_t size, uint64_t off,
			uint32_t minor, uint32_t op, uint32_t flags)
{
	int rc;
	trace_pxd_request(req->in.h.unique, size, off, minor, flags);

	switch (op) {
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,18,0) || (LINUX_VERSION_CODE >= KERNEL_VERSION(5,14,0) && (defined(__EL8__) || defined(__SUSE_EQ_SP5__)))
	case REQ_OP_WRITE_ZEROES:
#else
	case REQ_OP_WRITE_SAME:
#endif
		rc = pxd_write_same_request(req, size, off, minor, flags);
		break;
	case REQ_OP_WRITE:
		rc = pxd_write_request(req, size, off, minor, flags);
		break;
	case REQ_OP_READ:
		rc = pxd_read_request(req, size, off, minor, flags);
		break;
	case REQ_OP_DISCARD:
		rc = pxd_discard_request(req, size, off, minor, flags);
		break;
	case REQ_OP_FLUSH:
		rc = pxd_write_request(req, 0, 0, minor, REQ_FUA);
		break;
	default:
		printk(KERN_ERR"[%llu] REQ_OP_UNKNOWN(%#x): size=%d, off=%lld, minor=%d, flags=%#x\n",
			req->in.h.unique, op, size, off, minor, flags);
		return -1;
	}

	if (rc == 0) atomic_inc(&req->pxd_dev->ncount);
	return rc;
}

#else

static int pxd_request(struct fuse_req *req, uint32_t size, uint64_t off,
	uint32_t minor, uint32_t flags)
{
	int rc;
	trace_pxd_request(req->in.h.unique, size, off, minor, flags);

	switch (flags & (REQ_WRITE | REQ_DISCARD | REQ_WRITE_SAME)) {
	case REQ_WRITE:
		/* FALLTHROUGH */
	case (REQ_WRITE | REQ_WRITE_SAME):
		if (flags & REQ_WRITE_SAME)
			rc = pxd_write_same_request(req, size, off, minor, flags);
		else
			rc = pxd_write_request(req, size, off, minor, flags);
		break;
	case 0:
		rc = pxd_read_request(req, size, off, minor, flags);
		break;
	case REQ_DISCARD:
		/* FALLTHROUGH */
	case REQ_WRITE | REQ_DISCARD:
		rc = pxd_discard_request(req, size, off, minor, flags);
		break;
	default:
		printk(KERN_ERR"[%llu] REQ_OP_UNKNOWN(%#x): size=%d, off=%lld, minor=%d, flags=%#x\n",
			req->in.h.unique, flags, size, off, minor, flags);
		return -1;
	}

	if (rc == 0) atomic_inc(&req->pxd_dev->ncount);
	return rc;
}
#endif

static
bool pxd_process_ioswitch_complete(struct fuse_conn *fc, struct fuse_req *req,
	int status)
{
	struct pxd_device *pxd_dev = req->pxd_dev;
	struct list_head ios;
	unsigned long flags;

	if (atomic_cmpxchg(&pxd_dev->fp.ioswitch_active, 1, 0) == 0) {
		return false;
	}

	pxd_dev->fp.switch_uid = 0;
	INIT_LIST_HEAD(&ios);
	/// io path switch event completes with status.
	printk("device %llu completed ioswitch %d with status %d\n",
		pxd_dev->dev_id, req->in.h.opcode, status);

	if (req->in.h.opcode == PXD_FAILOVER_TO_USERSPACE) {
		// if the status is successful, then reissue IO to userspace
		// else fail IO to complete.
		disableFastPath(pxd_dev, true);

		spin_lock_irqsave(&pxd_dev->fp.fail_lock, flags);
		list_splice(&pxd_dev->fp.failQ, &ios);
		INIT_LIST_HEAD(&pxd_dev->fp.failQ);
		pxd_dev->fp.active_failover = false;
		spin_unlock_irqrestore(&pxd_dev->fp.fail_lock, flags);
	}

	// reopen the suspended device
	pxd_request_resume_internal(pxd_dev);

	// reissue any failed IOs from local list
	pxd_reissuefailQ(pxd_dev, &ios, status);

	return true;
}

static
int pxd_initiate_ioswitch(struct pxd_device *pxd_dev, int code)
{
	struct fuse_req *req;

	if (!fastpath_enabled(pxd_dev)) {
		return -EINVAL;
	}

	req = pxd_fuse_req(pxd_dev);
	if (IS_ERR_OR_NULL(req)) {
		return -ENOMEM;
	}

	req->pxd_dev = pxd_dev;
	req->bio = NULL;
	req->queue = pxd_dev->disk->queue;

	req->in.h.opcode = code;
	req->end = pxd_process_ioswitch_complete;
	pxd_req_misc(req, 0, 0, pxd_dev->minor, PXD_FLAGS_SYNC);

	pxd_dev->fp.switch_uid = req->in.h.unique;
	fuse_request_send_nowait(&pxd_dev->ctx->fc, req);
	return 0;
}

int pxd_initiate_failover(struct pxd_device *pxd_dev)
{
	int rc;

	if (!fastpath_active(pxd_dev)) {
		// already in native path
		return -EINVAL;
	}

	if (atomic_cmpxchg(&pxd_dev->fp.ioswitch_active, 0, 1) != 0) {
		return 0; // already initiated, skip it.
	}

	rc = pxd_request_suspend_internal(pxd_dev, false, true);
	if (rc) {
		atomic_set(&pxd_dev->fp.ioswitch_active, 0);
		return rc;
	}

	rc = pxd_initiate_ioswitch(pxd_dev, PXD_FAILOVER_TO_USERSPACE);
	if (rc) {
		pxd_request_resume(pxd_dev);
		atomic_set(&pxd_dev->fp.ioswitch_active, 0);
	}

	return rc;
}

int pxd_initiate_fallback(struct pxd_device *pxd_dev)
{
	int rc;

	if (fastpath_active(pxd_dev)) {
		// already in fast path
		return -EINVAL;
	}

	if (atomic_cmpxchg(&pxd_dev->fp.ioswitch_active, 0, 1) != 0) {
		return -EBUSY;
	}

	rc = pxd_request_suspend_internal(pxd_dev, true, false);
	if (rc) {
		atomic_set(&pxd_dev->fp.ioswitch_active, 0);
		return rc;
	}

	rc = pxd_initiate_ioswitch(pxd_dev, PXD_FALLBACK_TO_KERNEL);
	if (rc) {
		pxd_request_resume_internal(pxd_dev);
		atomic_set(&pxd_dev->fp.ioswitch_active, 0);
	}

	return rc;
}

#ifdef __PXD_BIO_MAKEREQ__
// similar function to make_request_slowpath only optimized to ensure its a reroute
// from fastpath on IO fail.
void pxd_reroute_slowpath(struct request_queue *q, struct bio *bio)
{
	struct pxd_device *pxd_dev = q->queuedata;
	struct fuse_req *req;

	req = pxd_fuse_req(pxd_dev);
	if (IS_ERR_OR_NULL(req)) {
		bio_io_error(bio);
		return;
	}

	req->pxd_dev = pxd_dev;
	req->bio = bio;
	req->queue = q;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,8,0) || defined(REQ_PREFLUSH)
	if (pxd_request(req, BIO_SIZE(bio), BIO_SECTOR(bio) * SECTOR_SIZE,
		pxd_dev->minor, bio_op(bio), bio->bi_opf)) {
#else
	if (pxd_request(req, BIO_SIZE(bio), BIO_SECTOR(bio) * SECTOR_SIZE,
		    pxd_dev->minor, bio->bi_rw)) {
#endif
		fuse_request_free(req);
		bio_io_error(bio);
		return;
	}

	fuse_request_send_nowait(&pxd_dev->ctx->fc, req);
}
#endif

#ifdef __PXD_BIO_BLKMQ__
#if !defined(__PX_BLKMQ__)
void pxdmq_reroute_slowpath(struct fuse_req *req)
{
    struct pxd_device *pxd_dev = req->pxd_dev;
    struct request *rq = req->rq;

    BUG_ON(pxd_dev->fp.fastpath);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 8, 0) || defined(REQ_PREFLUSH)
    if (pxd_request(req, blk_rq_bytes(rq), blk_rq_pos(rq) * SECTOR_SIZE,
                  pxd_dev->minor, req_op(rq), rq->cmd_flags)) {
#else
    if (pxd_request(req, blk_rq_bytes(rq), blk_rq_pos(rq) * SECTOR_SIZE,
                  pxd_dev->minor, rq->cmd_flags)) {
#endif
        fuse_request_free(req);
        blk_end_request(rq, -EIO, blk_rq_bytes(rq));
        return;
    }
    fuse_request_send_nowait(&pxd_dev->ctx->fc, req);
}


static void pxd_rq_fn(struct request_queue *q)
{
	struct pxd_device *pxd_dev = q->queuedata;
	struct fuse_req *req;
	struct fuse_conn *fc = &pxd_dev->ctx->fc;

	for (;;) {
		struct request *rq;

		/* Fetch request from block layer. */
		rq = blk_fetch_request(q);
		if (!rq)
			break;

		/* Filter out block requests we don't understand. */
		if (BLK_RQ_IS_PASSTHROUGH(rq) || !READ_ONCE(fc->allow_disconnected)) {
			__blk_end_request_all(rq, 0);
			continue;
		}
		spin_unlock_irq(&pxd_dev->qlock);
		pxd_printk("%s: dev m %d g %lld %s at %ld len %d bytes %d pages "
			"flags  %llx\n", __func__,
			pxd_dev->minor, pxd_dev->dev_id,
			rq_data_dir(rq) == WRITE ? "wr" : "rd",
			blk_rq_pos(rq) * SECTOR_SIZE, blk_rq_bytes(rq),
			rq->nr_phys_segments, rq->cmd_flags);

		req = pxd_fuse_req(pxd_dev);
		if (IS_ERR_OR_NULL(req)) {
			spin_lock_irq(&pxd_dev->qlock);
			__blk_end_request_all(rq, -EIO);
			continue;
		}

		req->pxd_dev = pxd_dev;
		req->rq = rq;
		req->queue = q;

#ifdef __PX_FASTPATH__
{
		struct fp_root_context *fproot = &req->fproot;
		fp_root_context_init(fproot);
		if (pxd_dev->fp.fastpath) {
			// route through fastpath
			fastpath_queue_work(&fproot->work, false);
			spin_lock_irq(&pxd_dev->qlock);
			continue;
		}
}
#endif
		atomic_inc(&pxd_dev->fp.nslowPath);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(4,8,0) || defined(REQ_PREFLUSH)
		if (pxd_request(req, blk_rq_bytes(rq), blk_rq_pos(rq) * SECTOR_SIZE,
			    pxd_dev->minor, req_op(rq), rq->cmd_flags)) {
#else
		if (pxd_request(req, blk_rq_bytes(rq), blk_rq_pos(rq) * SECTOR_SIZE,
			    pxd_dev->minor, rq->cmd_flags)) {
#endif
			fuse_request_free(req);
			spin_lock_irq(&pxd_dev->qlock);
			__blk_end_request_all(rq, -EIO);
			continue;
		}

		fuse_request_send_nowait(fc, req);
		spin_lock_irq(&pxd_dev->qlock);
	}
}
#else

void pxdmq_reroute_slowpath(struct fuse_req *req)
{
    struct pxd_device *pxd_dev = req->pxd_dev;
    struct request *rq = req->rq;

    BUG_ON(pxd_dev->fp.fastpath);

    if (pxd_request(req, blk_rq_bytes(rq), blk_rq_pos(rq) * SECTOR_SIZE,
        pxd_dev->minor, req_op(rq), rq->cmd_flags)) {
        blk_mq_end_request(rq, BLK_STS_IOERR);
        return;
    }

    fuse_request_send_nowait(&pxd_dev->ctx->fc, req);
}

static blk_status_t pxd_queue_rq(struct blk_mq_hw_ctx *hctx,
		const struct blk_mq_queue_data *bd)
{
	struct request *rq = bd->rq;
	struct pxd_device *pxd_dev = rq->q->queuedata;
	struct fuse_req *req = blk_mq_rq_to_pdu(rq);
	struct fuse_conn *fc = &pxd_dev->ctx->fc;

	if (BLK_RQ_IS_PASSTHROUGH(rq) || !READ_ONCE(fc->allow_disconnected))
		return BLK_STS_IOERR;

	pxd_printk("%s: dev m %d g %lld %s at %ld len %d bytes %d pages "
		   "flags  %x\n", __func__,
		pxd_dev->minor, pxd_dev->dev_id,
		rq_data_dir(rq) == WRITE ? "wr" : "rd",
		blk_rq_pos(rq) * SECTOR_SIZE, blk_rq_bytes(rq),
		rq->nr_phys_segments, rq->cmd_flags);

	fuse_request_init(req);

	blk_mq_start_request(rq);

	req->pxd_dev = pxd_dev;
	req->rq = rq;

#ifdef __PX_FASTPATH__
{
	struct fp_root_context *fproot = &req->fproot;
	fp_root_context_init(fproot);
	if (pxd_dev->fp.fastpath) {
		// route through fastpath
		// while in blkmq mode: cannot directly process IO from this thread... involves
		// recursive BIO submission to the backing devices, causing deadlock.
		fastpath_queue_work(&fproot->work, false);
		return BLK_STS_OK;
	}
}
#endif
	atomic_inc(&pxd_dev->fp.nslowPath);

	if (pxd_request(req, blk_rq_bytes(rq), blk_rq_pos(rq) * SECTOR_SIZE,
		pxd_dev->minor, req_op(rq), rq->cmd_flags)) {
		return BLK_STS_IOERR;
	}

	fuse_request_send_nowait(fc, req);

	return BLK_STS_OK;
}

static const struct blk_mq_ops pxd_mq_ops = {
	.queue_rq       = pxd_queue_rq,
};
#endif /* __PX_BLKMQ__ */
#endif /* __PXD_BIO_BLKMQ__ */

static int pxd_init_disk(struct pxd_device *pxd_dev)
{
	struct gendisk *disk;
	struct request_queue *q;
#ifdef __PX_BLKMQ__
	int err = 0;
#endif

	// only fastpath uses direct block io process bypassing request queuing
#ifndef __PX_FASTPATH__
	if (fastpath_enabled(pxd_dev)) {
		printk(KERN_NOTICE"PX driver does not support fastpath, disabling it\n");
		pxd_dev->fastpath = false;
	}
#endif

#ifdef __PXD_BIO_MAKEREQ__
		pxd_printk("adding disk as makereq device %llu", pxd_dev->dev_id);
		/* Create gendisk info. */
		disk = alloc_disk(1);
		if (!disk) {
			return -ENOMEM;
		}
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,9,0)
	 	q = blk_alloc_queue(NUMA_NO_NODE);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(5,7,0)
		q = blk_alloc_queue(pxd_bio_make_request_entryfn, NUMA_NO_NODE);
#elif LINUX_VERSION_CODE == KERNEL_VERSION(4,18,0) && defined(__EL8__) && defined(QUEUE_FLAG_NOWAIT)
        q = blk_alloc_queue_rh(pxd_bio_make_request_entryfn, NUMA_NO_NODE);
#else
		q = blk_alloc_queue(GFP_KERNEL);
#endif
		if (!q) {
			put_disk(disk);
			return -ENOMEM;
		}
#if LINUX_VERSION_CODE < KERNEL_VERSION(5,9,0)
		// add hooks to control congestion only while using fastpath
		PXD_SETUP_CONGESTION_HOOK(q->backing_dev_info, pxd_device_congested, pxd_dev);
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(5,7,0) && !defined(QUEUE_FLAG_NOWAIT)
		blk_queue_make_request(q, pxd_bio_make_request_entryfn);
#endif

#else /* ! __PXD_BIO_MAKEREQ__ */

#ifdef __PX_BLKMQ__
	  memset(&pxd_dev->tag_set, 0, sizeof(pxd_dev->tag_set));
	  pxd_dev->tag_set.ops = &pxd_mq_ops;
	  pxd_dev->tag_set.queue_depth = pxd_dev->queue_depth;
	  pxd_dev->tag_set.numa_node = NUMA_NO_NODE;
	  pxd_dev->tag_set.flags = BLK_MQ_F_SHOULD_MERGE;
	  pxd_dev->tag_set.nr_hw_queues = num_online_nodes() * pxd_num_fpthreads;
	  pxd_dev->tag_set.cmd_size = sizeof(struct fuse_req);

	  err = blk_mq_alloc_tag_set(&pxd_dev->tag_set);
	  if (err) {
		return err;
	  }

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,14,0)

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,9,0)
	  struct queue_limits lim = {
		  .logical_block_size = PXD_LBS,
		  .physical_block_size = PXD_LBS,
		  .max_segment_size = SEGMENT_SIZE,
		  .max_segments = SEGMENT_SIZE / PXD_LBS,
		  .max_hw_sectors = SEGMENT_SIZE / SECTOR_SIZE,
		  .discard_alignment = PXD_MAX_DISCARD_GRANULARITY,
		  .discard_granularity = PXD_MAX_DISCARD_GRANULARITY,
		  .io_min = PXD_LBS,
		  .io_opt = PXD_LBS,
		  .max_hw_discard_sectors = pxd_dev->discard_size / SECTOR_SIZE,
		  .max_discard_sectors = pxd_dev->discard_size / SECTOR_SIZE
	  };
	  disk = blk_mq_alloc_disk(&pxd_dev->tag_set, &lim, pxd_dev);
#else
	  disk = blk_mq_alloc_disk(&pxd_dev->tag_set, pxd_dev);
#endif

	  if (IS_ERR(disk)) {
		blk_mq_free_tag_set(&pxd_dev->tag_set);
		return PTR_ERR(disk);
	  }
	  q = disk->queue;
	  disk->minors = 1;
#else
	  /* Create gendisk info. */
	  disk = alloc_disk(1);
	  if (!disk) {
		return -ENOMEM;
	  }
	  q = blk_mq_init_queue(&pxd_dev->tag_set);
	  if (IS_ERR(q)) {
		blk_mq_free_tag_set(&pxd_dev->tag_set);
		put_disk(disk);
		return PTR_ERR(q);
	  }
#endif
#else /* !__PX_BLKMQ__		  */
	  /* Create gendisk info. */
	  disk = alloc_disk(1);
	  if (!disk) {
		return -ENOMEM;
	  }
	  q = blk_init_queue(pxd_rq_fn, &pxd_dev->qlock);
	  if (!q) {
		put_disk(disk);
		return -ENOMEM;
	  }
#endif /* __PX_BLKMQ__ */
#endif /* __PXD_BIO_MAKEREQ__ */

	// Disk and queue initialization
	snprintf(disk->disk_name, sizeof(disk->disk_name),
		 PXD_DEV"%llu", pxd_dev->dev_id);
	disk->major = pxd_dev->major;
	disk->minors = 1;
	disk->first_minor = pxd_dev->minor;

#if defined(GENHD_FL_NO_PART) || LINUX_VERSION_CODE >= KERNEL_VERSION(5,17,0) || (LINUX_VERSION_CODE == KERNEL_VERSION(5,14,0) && defined(__EL8__) && !defined(BLKDEV_DISCARD_SECURE)) || (LINUX_VERSION_CODE >= KERNEL_VERSION(5,14,0) && defined(__SUSE_EQ_SP5__))
	disk->flags |= GENHD_FL_NO_PART;
#else
	disk->flags |= GENHD_FL_EXT_DEVT | GENHD_FL_NO_PART_SCAN;
#endif

	disk->fops = get_bd_fpops();
	disk->private_data = pxd_dev;
	set_capacity(disk, pxd_dev->size / SECTOR_SIZE);

	blk_queue_max_hw_sectors(q, SEGMENT_SIZE / SECTOR_SIZE);
	blk_queue_max_segment_size(q, SEGMENT_SIZE);
	blk_queue_max_segments(q, (SEGMENT_SIZE / PXD_LBS));
	blk_queue_io_min(q, PXD_LBS);
	blk_queue_io_opt(q, PXD_LBS);
	blk_queue_logical_block_size(q, PXD_LBS);
	blk_queue_physical_block_size(q, PXD_LBS);

#if LINUX_VERSION_CODE < KERNEL_VERSION(5,19,0)
#if defined(__EL8__) || defined(__SUSE_EQ_SP5__)

#if defined(QUEUE_FLAG_DISCARD)
	/* Enable discard support. */
	QUEUE_FLAG_SET(QUEUE_FLAG_DISCARD,q);
#endif

#else                                                         // #else for defined(__EL8__) || defined(__SUSE_EQ_SP5__)
	/* Enable discard support. */
	QUEUE_FLAG_SET(QUEUE_FLAG_DISCARD,q);
#endif                                                        // #endif for defined(__EL8__) || defined(__SUSE_EQ_SP5__)
#endif                                                        // #endif for LINUX_VERSION_CODE < KERNEL_VERSION(5,19,0)

    q->limits.discard_granularity = PXD_MAX_DISCARD_GRANULARITY;
    q->limits.discard_alignment = PXD_MAX_DISCARD_GRANULARITY;
	if (pxd_dev->discard_size < SECTOR_SIZE)
		q->limits.max_discard_sectors = SEGMENT_SIZE / SECTOR_SIZE;
	else
		q->limits.max_discard_sectors = pxd_dev->discard_size / SECTOR_SIZE;

#if LINUX_VERSION_CODE < KERNEL_VERSION(4,12,0)
	q->limits.discard_zeroes_data = 1;
#endif

	/* Enable flush support. */
	BLK_QUEUE_FLUSH(q);

	/* adjust queue limits to be compatible with backing device */
#ifdef __PXD_BIO_MAKEREQ__
	pxd_fastpath_adjust_limits(pxd_dev, q);
#endif

	disk->queue = q;
	q->queuedata = pxd_dev;
	pxd_dev->disk = disk;

#if defined __PX_BLKMQ__ && !defined __PXD_BIO_MAKEREQ__
	blk_mq_freeze_queue(q);
#endif

	return 0;
}

static void pxd_free_disk(struct pxd_device *pxd_dev)
{
	struct gendisk *disk = pxd_dev->disk;

	if (!disk)
		return;

	pxd_fastpath_cleanup(pxd_dev);
	pxd_dev->disk = NULL;
	if (disk) {
		del_gendisk(disk);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6,0,0) || (LINUX_VERSION_CODE >= KERNEL_VERSION(5,14,0) && (defined(__EL8__) || defined(__SUSE_EQ_SP5__)))
		if (disk->queue) {
			put_disk(disk);
		}
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(5,14,0)
		if (disk->queue) {
			blk_cleanup_disk(disk);
		}
#else
		if (disk->queue) {
			blk_cleanup_queue(disk->queue);
		}
		put_disk(disk);
#endif

#if defined(__PXD_BIO_BLKMQ__) && defined(__PX_BLKMQ__)
		blk_mq_free_tag_set(&pxd_dev->tag_set);
#endif
	}
}

struct pxd_device* find_pxd_device(struct pxd_context *ctx, uint64_t dev_id)
{
	struct pxd_device *pxd_dev_itr, *pxd_dev;

	pxd_dev = NULL;
	spin_lock(&ctx->lock);
	list_for_each_entry(pxd_dev_itr, &ctx->list, node) {
		if (pxd_dev_itr->dev_id == dev_id) {
			pxd_dev = pxd_dev_itr;
			break;
		}
	}
	spin_unlock(&ctx->lock);

	return pxd_dev;
}

static int __pxd_update_path(struct pxd_device *pxd_dev, struct pxd_update_path_out *update_path);
ssize_t pxd_add(struct fuse_conn *fc, struct pxd_add_ext_out *add)
{
	struct pxd_context *ctx = container_of(fc, struct pxd_context, fc);
	struct pxd_device *pxd_dev = NULL;
	struct pxd_device *pxd_dev_itr;
	int new_minor;
	int err;

	err = -ENOMEM;
	if (ctx->num_devices >= PXD_MAX_DEVICES) {
		printk(KERN_ERR "Too many devices attached..\n");
		goto out_module;
	}

	// if device already exists, then return it
	pxd_dev = find_pxd_device(ctx, add->dev_id);
	if (pxd_dev) {
		if (add->enable_fp && add->paths.count > 0) {
			__pxd_update_path(pxd_dev, &add->paths);
		} else {
			disableFastPath(pxd_dev, false);
		}
		return pxd_dev->minor | (fastpath_active(pxd_dev) << MINORBITS);
	}

	pxd_dev = kzalloc(sizeof(*pxd_dev), GFP_KERNEL);
	if (!pxd_dev)
		goto out_module;

	pxd_mem_printk("device %llu allocated at %px\n", add->dev_id, pxd_dev);

	pxd_dev->magic = PXD_DEV_MAGIC;
	pxd_dev->exported = false;
	pxd_dev->removing = false;
	spin_lock_init(&pxd_dev->lock);
	spin_lock_init(&pxd_dev->qlock);

	new_minor = ida_simple_get(&pxd_minor_ida,
					1, 1 << MINORBITS,
					GFP_KERNEL);
	if (new_minor < 0) {
		err = new_minor;
		goto out_module;
	}

	pxd_dev->dev_id = add->dev_id;
	pxd_dev->major = pxd_major;
	pxd_dev->minor = new_minor;
	pxd_dev->ctx = ctx;
	pxd_dev->connected = true; // fuse slow path connection
	pxd_dev->size = add->size;
	pxd_dev->mode = add->open_mode;
	pxd_dev->fastpath = add->enable_fp;

	// congestion init
	init_waitqueue_head(&pxd_dev->suspend_wq);
	init_waitqueue_head(&pxd_dev->remove_wait);
	// hard coded congestion limits within driver
	atomic_set(&pxd_dev->congested, 0);
	pxd_dev->qdepth = DEFAULT_CONGESTION_THRESHOLD;
	pxd_dev->nr_congestion_on = 0;
	pxd_dev->nr_congestion_off = 0;
	atomic_set(&pxd_dev->ncount, 0);

	pxd_dev->queue_depth = PXD_MAX_QDEPTH;
	if (add->queue_depth < 0 || add->queue_depth > PXD_MAX_QDEPTH) {
		err = -EINVAL;
		goto out_module;
	} else if (add->queue_depth != 0) {
		pxd_dev->queue_depth = add->queue_depth;
	}

	if (add->discard_size < SECTOR_SIZE)
		pxd_dev->discard_size = SEGMENT_SIZE;
	else
		pxd_dev->discard_size = add->discard_size;

	printk(KERN_INFO"Device %llu added %px with mode %#x fastpath %d npath %lu\n",
			add->dev_id, pxd_dev, add->open_mode, add->enable_fp, add->paths.count);

	// initializes fastpath context part of pxd_dev
	err = pxd_fastpath_init(pxd_dev);
	if (err)
		goto out_id;

	if (fastpath_enabled(pxd_dev)) {
		err = pxd_init_fastpath_target(pxd_dev, &add->paths);
		if (err) {
			goto out_fp;
		}
	}

	spin_lock(&ctx->lock);
	list_for_each_entry(pxd_dev_itr, &ctx->list, node) {
		if (pxd_dev_itr->dev_id == add->dev_id) {
			err = -EEXIST;
			spin_unlock(&ctx->lock);
			goto out_fp;
		}
	}

	list_add(&pxd_dev->node, &ctx->list);
	++ctx->num_devices;
	spin_unlock(&ctx->lock);
	return pxd_dev->minor | (fastpath_active(pxd_dev) << MINORBITS);

out_fp:
	pxd_fastpath_cleanup(pxd_dev);
out_id:
	ida_simple_remove(&pxd_minor_ida, new_minor);
out_module:
	if (pxd_dev)
		kfree(pxd_dev);

	return err;
}

ssize_t pxd_export(struct fuse_conn *fc, uint64_t dev_id)
{
	struct pxd_context *ctx = container_of(fc, struct pxd_context, fc);
	struct pxd_device *pxd_dev = find_pxd_device(ctx, dev_id);
	int err = 0;

	if (!pxd_dev) {
		return -ENOENT;
	}

	spin_lock(&pxd_dev->lock);
	if (pxd_dev->exported) {
		spin_unlock(&pxd_dev->lock);
		return 0;
	}
	spin_unlock(&pxd_dev->lock);

	if (!try_module_get(THIS_MODULE)) {
		err = -ENODEV;
		goto cleanup;
	}

	err = pxd_init_disk(pxd_dev);
	if (err) {
		module_put(THIS_MODULE);
		goto cleanup;
	}

	err = pxd_bus_add_dev(pxd_dev);
	if (err) {
		pxd_free_disk(pxd_dev);
		module_put(THIS_MODULE);
		goto cleanup;
	}


#if LINUX_VERSION_CODE > KERNEL_VERSION(5,15,50) || (LINUX_VERSION_CODE >= KERNEL_VERSION(5,14,0) && (defined(__EL8__) || defined(__SUSE_EQ_SP5__)))
	err = device_add_disk(&pxd_dev->dev, pxd_dev->disk, NULL);
#elif LINUX_VERSION_CODE >= KERNEL_VERSION(5,13,0)
	device_add_disk(&pxd_dev->dev, pxd_dev->disk, NULL);
#else
	add_disk(pxd_dev->disk);
#endif
	if (err) {
		device_unregister(&pxd_dev->dev);
		pxd_free_disk(pxd_dev);
		module_put(THIS_MODULE);
		goto cleanup;
	}

	spin_lock(&pxd_dev->lock);
	pxd_dev->exported = true;
	spin_unlock(&pxd_dev->lock);
#if defined __PX_BLKMQ__ && !defined __PXD_BIO_MAKEREQ__
	blk_mq_unfreeze_queue(pxd_dev->disk->queue);
#endif
	return 0;
cleanup:
    spin_lock(&ctx->lock);
    spin_lock(&pxd_dev->lock);
    list_del(&pxd_dev->node);
    --ctx->num_devices;
	pxd_dev->exported = false;
    ida_simple_remove(&pxd_minor_ida, pxd_dev->minor);
    spin_unlock(&pxd_dev->lock);
    spin_unlock(&ctx->lock);
	kfree(pxd_dev);
	return err;
}

static void pxd_finish_remove(struct work_struct *work)
{
	struct pxd_device *pxd_dev = container_of(work, struct pxd_device, remove_work);

	pr_info("%s: dev %llu\n", __func__, pxd_dev->dev_id);

	pxd_fastpath_reset_device(pxd_dev);

	/* Make sure the req_fn isn't called anymore even if the device hangs around */
	if (pxd_dev->disk && pxd_dev->disk->queue){
#ifndef __PX_BLKMQ__
		mutex_lock(&pxd_dev->disk->queue->sysfs_lock);

		QUEUE_FLAG_SET(QUEUE_FLAG_DYING, pxd_dev->disk->queue);

		mutex_unlock(&pxd_dev->disk->queue->sysfs_lock);
#else
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,15,25) || (LINUX_VERSION_CODE >= KERNEL_VERSION(5,14,0) && ((defined(__EL8__) && !defined(QUEUE_FLAG_DEAD)) || defined(__SUSE_EQ_SP5__)))
	// del_gendisk will try to fsync device
	// so freeze queue and then *mark queue dead* to ensure no new reqs
	// gets accepted.
    blk_freeze_queue_start(pxd_dev->disk->queue);
    blk_mark_disk_dead(pxd_dev->disk);
#elif LINUX_VERSION_CODE < KERNEL_VERSION(5,13,0)
	// del_gendisk will not submit any new IO.
	// so freeze queue and then queue dying, to ensure no new reqs
	// gets accepted.
    blk_freeze_queue_start(pxd_dev->disk->queue);
    blk_set_queue_dying(pxd_dev->disk->queue);
#endif
	// kernel bug here, disk deletion code, fsync IO, checking for disk dead while queue is marked dying,
	// which will never happen, and the disk deletion code is stuck in deadlock.
	// in this case, allow IOs to go to the queue and let px-storage handle IO
	// on the device, so fsync can pass, and del_gendisk can proceed
	// to safely delete the device.
#endif
	}

	pxd_free_disk(pxd_dev);

	get_device(&pxd_dev->dev);
	device_unregister(&pxd_dev->dev);

	spin_lock(&pxd_dev->ctx->lock);
	spin_lock(&pxd_dev->lock);
	--pxd_dev->ctx->num_devices;
	pxd_dev->exported = false;
	list_del(&pxd_dev->node);
	wake_up_all(&pxd_dev->remove_wait);
	spin_unlock(&pxd_dev->lock);
	spin_unlock(&pxd_dev->ctx->lock);

	put_device(&pxd_dev->dev);

	module_put(THIS_MODULE);
}

ssize_t pxd_remove(struct fuse_conn *fc, struct pxd_remove_out *remove)
{
	struct pxd_context *ctx = container_of(fc, struct pxd_context, fc);
	int err;
	struct pxd_device *pxd_dev;
	long remtimeo;
	DEFINE_WAIT(wait);

	spin_lock(&ctx->lock);
	list_for_each_entry(pxd_dev, &ctx->list, node) {
		if (pxd_dev->dev_id == remove->dev_id) {
			spin_lock(&pxd_dev->lock);
			break;
		}
	}
	if (&pxd_dev->node == &ctx->list) {
		err = -ENOENT;
		goto out;
	}
	if (!pxd_dev->exported) {
		err = -ENOENT;
		goto out_lock;
	}

	if (pxd_dev->open_count && !remove->force) {
		err = -EBUSY;
		goto out_lock;
	}

	if (!pxd_dev->removing) {
		pxd_dev->removing = true;
		INIT_WORK(&pxd_dev->remove_work, pxd_finish_remove);
		schedule_work(&pxd_dev->remove_work);
	}
	get_device(&pxd_dev->dev);
	prepare_to_wait(&pxd_dev->remove_wait, &wait, TASK_INTERRUPTIBLE);
	spin_unlock(&pxd_dev->lock);
	spin_unlock(&ctx->lock);
	// future proof against device removal bugs
	// schedule and forget if not done in reasonable time.
	remtimeo = schedule_timeout(msecs_to_jiffies(500));
	finish_wait(&pxd_dev->remove_wait, &wait);
	if (remtimeo == 0) {
		pr_warn("remove device %llu scheduled but timedout waiting to complete",
				remove->dev_id);
	}
	put_device(&pxd_dev->dev);
	return 0;
out_lock:
	spin_unlock(&pxd_dev->lock);
out:
	spin_unlock(&ctx->lock);
	pr_err("remove device %llu failed %d\n", remove->dev_id, err);
	return err;
}

ssize_t pxd_update_size(struct fuse_conn *fc, struct pxd_update_size *update_size)
{
	return -EOPNOTSUPP;
}

ssize_t pxd_ioc_update_size(struct fuse_conn *fc, struct pxd_update_size *update_size)
{
	bool found = false;
	struct pxd_context *ctx = container_of(fc, struct pxd_context, fc);
	int err;
	struct pxd_device *pxd_dev;

	spin_lock(&ctx->lock);
	list_for_each_entry(pxd_dev, &ctx->list, node) {
		if ((pxd_dev->dev_id == update_size->dev_id) && !pxd_dev->removing) {
			spin_lock(&pxd_dev->lock);
			found = true;
			break;
		}
	}
	spin_unlock(&ctx->lock);

	if (!found) {
		err = -ENOENT;
		goto out;
	}

	if (update_size->size < pxd_dev->size) {
		spin_unlock(&pxd_dev->lock);
		err = -EINVAL;
		goto out;
	}
	(void)get_device(&pxd_dev->dev);

#if LINUX_VERSION_CODE < KERNEL_VERSION(5,11,0)
	set_capacity(pxd_dev->disk, update_size->size / SECTOR_SIZE);
#else
	// set_capacity is sufficient for modifying disk size from 5.11 onwards
	set_capacity_and_notify(pxd_dev->disk, update_size->size / SECTOR_SIZE);
#endif
	spin_unlock(&pxd_dev->lock);

	// set_capacity is sufficient for modifying disk size from 5.11 onwards
#if LINUX_VERSION_CODE < KERNEL_VERSION(5,11,0)
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5,10,0) || (defined(__EL8__) && defined(GD_READ_ONLY))
	revalidate_disk_size(pxd_dev->disk, true);
#else
	err = revalidate_disk(pxd_dev->disk);
	BUG_ON(err);
#endif
#endif
	put_device(&pxd_dev->dev);

	return 0;
out:
	return err;
}

ssize_t pxd_read_init(struct fuse_conn *fc, struct iov_iter *iter)
{
	size_t copied = 0;
	struct pxd_context *ctx = container_of(fc, struct pxd_context, fc);
	struct pxd_device *pxd_dev;
	struct pxd_init_in pxd_init;

	// Taking context lock is unnecessary, as this gets called during
	// userspace init only once, no changes to the pxd block device
	// layer is expected during this window.

	pxd_init.num_devices = ctx->num_devices;
	pxd_init.version = PXD_VERSION;

	if (copy_to_iter(&pxd_init, sizeof(pxd_init), iter) != sizeof(pxd_init)) {
		printk(KERN_ERR "%s: copy pxd_init error\n", __func__);
		goto copy_error;
	}

	copied += sizeof(pxd_init);

	list_for_each_entry(pxd_dev, &ctx->list, node) {
		struct pxd_dev_id id = {0}; // initialize all fields to zero
		id.dev_id = pxd_dev->dev_id;
		id.local_minor = pxd_dev->minor;
		id.fastpath = 0;
#ifdef __PXD_BIO_MAKEREQ__
		id.blkmq_device = 0;
#else
		id.blkmq_device = 1;
#endif
		id.suspend = 0;
		// resume from userspace IO suspends after px restarts
		pxd_request_resume(pxd_dev);
		if (pxd_dev->fp.fastpath) id.fastpath = 1;
		if (copy_to_iter(&id, sizeof(id), iter) != sizeof(id)) {
			printk(KERN_ERR "%s: copy dev id error copied %ld\n", __func__,
				copied);
			goto copy_error;
		}
		copied += sizeof(id);
	}

	printk(KERN_INFO "%s: pxd-control-%d init OK %d devs version %d\n", __func__,
		ctx->id, pxd_init.num_devices, pxd_init.version);

	return copied;

copy_error:
	return -EFAULT;
}


static int __pxd_update_path(struct pxd_device *pxd_dev, struct pxd_update_path_out *update_path)
{
	if (!fastpath_enabled(pxd_dev)) {
		printk(KERN_WARNING"%llu: block device registered for native path - cannot update for fastpath\n", pxd_dev->dev_id);
		return -EINVAL;
	} else if (fastpath_active(pxd_dev)) {
		printk(KERN_ERR"%llu: device already in fast path - cannot update\n", pxd_dev->dev_id);
		return -EINVAL;
	}
	return pxd_init_fastpath_target(pxd_dev, update_path);
}

static void _pxd_setup(struct pxd_device *pxd_dev, bool enable)
{
	if (!enable) {
		printk(KERN_NOTICE "device %llu called to disable IO\n", pxd_dev->dev_id);
		spin_lock(&pxd_dev->lock);
		pxd_dev->connected = false;
		spin_unlock(&pxd_dev->lock);
		pxd_fastpath_reset_device(pxd_dev);
	} else {
		printk(KERN_NOTICE "device %llu called to enable IO\n", pxd_dev->dev_id);
		spin_lock(&pxd_dev->lock);
		pxd_dev->connected = true;
		spin_unlock(&pxd_dev->lock);
	}
}

static void pxdctx_set_connected(struct pxd_context *ctx, bool enable)
{
	struct list_head *cur;
	spin_lock(&ctx->lock);
	list_for_each(cur, &ctx->list) {
		struct pxd_device *pxd_dev = container_of(cur, struct pxd_device, node);

		_pxd_setup(pxd_dev, enable);
	}
	spin_unlock(&ctx->lock);
}

static struct bus_type pxd_bus_type = {
	.name		= "pxd",
};

static void pxd_root_dev_release(struct device *dev)
{
}

static struct device pxd_root_dev = {
	.init_name =    "pxd",
	.release =      pxd_root_dev_release,
};

static struct pxd_device *dev_to_pxd_dev(struct device *dev)
{
	return container_of(dev, struct pxd_device, dev);
}

static ssize_t pxd_size_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct pxd_device *pxd_dev = dev_to_pxd_dev(dev);

	return sprintf(buf, "%llu\n",
		(unsigned long long)pxd_dev->size);
}

static ssize_t pxd_major_show(struct device *dev,
			 struct device_attribute *attr, char *buf)
{
	struct pxd_device *pxd_dev = dev_to_pxd_dev(dev);

	return sprintf(buf, "%llu\n",
			(unsigned long long)pxd_dev->major);
}

static ssize_t pxd_minor_show(struct device *dev,
			 struct device_attribute *attr, char *buf)
{
	struct pxd_device *pxd_dev = dev_to_pxd_dev(dev);

	return sprintf(buf, "%llu\n",
			(unsigned long long)pxd_dev->minor);
}

static ssize_t pxd_timeout_show(struct device *dev,
			 struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", pxd_timeout_secs);
}

static ssize_t pxd_timeout_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct pxd_device *pxd_dev = dev_to_pxd_dev(dev);
	uint32_t new_timeout_secs = 0;
	struct pxd_context *ctx = pxd_dev->ctx;

	if (ctx == NULL)
		return -ENXIO;

	sscanf(buf, "%u", &new_timeout_secs);
	if (new_timeout_secs < PXD_TIMER_SECS_MIN ||
			new_timeout_secs > PXD_TIMER_SECS_MAX) {
		return -EINVAL;
	}

	if (!READ_ONCE(ctx->fc.connected)) {
		cancel_delayed_work_sync(&ctx->abort_work);
	}
	spin_lock(&ctx->lock);
	pxd_timeout_secs = new_timeout_secs;
	if (!READ_ONCE(ctx->fc.connected)) {
		schedule_delayed_work(&ctx->abort_work, pxd_timeout_secs * HZ);
	}
	spin_unlock(&ctx->lock);

	return count;
}

static ssize_t pxd_active_show(struct device *dev,
					 struct device_attribute *attr, char *buf)
{
	struct pxd_device *pxd_dev = dev_to_pxd_dev(dev);
	char *cp = buf;
	int ncount;
	int available = PAGE_SIZE - 1;
	int i;

	ncount = snprintf(cp, available, "active/complete: %u/%u, failed: %u, [write: %u, flush: %u(nop: %u), fua: %u, discard: %u, preflush: %u], switched: %u, slowpath: %u\n",
                atomic_read(&pxd_dev->ncount), atomic_read(&pxd_dev->fp.ncomplete),
		atomic_read(&pxd_dev->fp.nerror),
		atomic_read(&pxd_dev->fp.nio_write),
		atomic_read(&pxd_dev->fp.nio_flush), atomic_read(&pxd_dev->fp.nio_flush_nop),
		atomic_read(&pxd_dev->fp.nio_fua), atomic_read(&pxd_dev->fp.nio_discard),
		atomic_read(&pxd_dev->fp.nio_preflush),
		atomic_read(&pxd_dev->fp.nswitch), atomic_read(&pxd_dev->fp.nslowPath));

	cp += ncount;
	available -= ncount;
	for (i = 0; i < pxd_dev->fp.nfd; i++) {
		size_t tmp = snprintf(cp, available, "%s\n", pxd_dev->fp.device_path[i]);
		cp += tmp;
		available -= tmp;
		ncount += tmp;
	}

	return ncount;
}

static ssize_t pxd_mode_show(struct device *dev,
					 struct device_attribute *attr, char *buf)
{
	char modestr[32];
	struct pxd_device *pxd_dev = dev_to_pxd_dev(dev);

	decode_mode(pxd_dev->mode, modestr);
	return sprintf(buf, "mode: %#x/%s\n", pxd_dev->mode, modestr);
}

static ssize_t pxd_congestion_show(struct device *dev,
					 struct device_attribute *attr, char *buf)
{
	struct pxd_device *pxd_dev = dev_to_pxd_dev(dev);

	return sprintf(buf, "congested: %s (%d/%d)\n",
			atomic_read(&pxd_dev->congested) ? "yes" : "no",
			pxd_dev->nr_congestion_on,
			pxd_dev->nr_congestion_off);
}

static ssize_t pxd_congestion_set(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct pxd_device *pxd_dev = dev_to_pxd_dev(dev);
	int thresh;

	sscanf(buf, "%d", &thresh);

	if (thresh < 0) {
		thresh = pxd_dev->qdepth;
	}

	if (thresh > MAX_CONGESTION_THRESHOLD) {
		thresh = MAX_CONGESTION_THRESHOLD;
	}

	spin_lock(&pxd_dev->lock);
	pxd_dev->qdepth = thresh;
	spin_unlock(&pxd_dev->lock);

	return count;
}

static ssize_t pxd_fastpath_state(struct device *dev,
					 struct device_attribute *attr, char *buf)
{
	struct pxd_device *pxd_dev = dev_to_pxd_dev(dev);
	return sprintf(buf, "%d\n", pxd_dev->fp.fastpath);
}

static char* __strtok_r(char *src, const char delim, char **saveptr) {
	char *curr;
	char *start;

	if (src) {
		start = src;
		*saveptr = NULL;
	} else {
		start = *saveptr;
	}
	curr = start;
	while (curr && *curr) {
		if (*curr == delim) {
			*saveptr = curr+1;
			*curr = '\0';

			return start;
		}
		curr++;
	}

	return start;
}

static void __strip_nl(const char *src, char *dst, int maxlen) {
	char *tmp;
	int len=strlen(src);


	if (!src || !dst) {
		return;
	}

	dst[0] = '\0';
	if (!len) {
		return;
	}

	if (len >= maxlen) {
		// to accomodate null
		printk(KERN_WARNING"stripping newline output buffer overflow.. src %d(%s), dst %d\n",
				len, src, maxlen);
		len = maxlen - 1;
	}

	// leading space
	while (src && *src) {
		if (!isspace(*src) && !iscntrl(*src)) {
			memcpy(dst,src,len);
			dst[len]='\0';
			break;
		}
		src++;
		len--;
	}

	// trailing space
	tmp = dst + len - 1;
	while (len && *tmp) {
		if (isspace(*tmp) || iscntrl(*tmp)) {
			*tmp='\0';
			tmp--;
			len--;
			continue;
		}
		break;
	}

	printk(KERN_INFO"stripping newline src=(%s), dst=(%s), len=%d\n",
			src, dst, len);
}

static ssize_t pxd_fastpath_update(struct device *dev, struct device_attribute *attr,
					   const char *buf, size_t count)
{
	// format: path,path,path
	struct pxd_device *pxd_dev = dev_to_pxd_dev(dev);
	struct pxd_update_path_out update_out;
	const char delim = ',';
	char *token;
	char *saveptr = NULL;
	int i;
	char trimtoken[256];

	char *tmp = kzalloc(count, GFP_KERNEL);
	if (!tmp) {
		printk("No memory to process %lu bytes\n", count);
		return count;
	}
	memcpy(tmp, buf, count);

	i=0;
	token = __strtok_r(tmp, delim, &saveptr);
	for (i = 0; i < MAX_PXD_BACKING_DEVS && token; i++) {
		// strip the token of any newline/whitespace
		__strip_nl(token, trimtoken, sizeof(trimtoken));
		strncpy(update_out.devpath[i], trimtoken, MAX_PXD_DEVPATH_LEN);
		update_out.devpath[i][MAX_PXD_DEVPATH_LEN] = '\0';

		token = __strtok_r(0, delim, &saveptr);
	}
	update_out.count = i;
	update_out.can_failover = false;
	update_out.dev_id = pxd_dev->dev_id;

	__pxd_update_path(pxd_dev, &update_out);
	kfree(tmp);

	return count;
}

static ssize_t pxd_debug_show(struct device *dev,
                     struct device_attribute *attr, char *buf)
{
	struct pxd_device *pxd_dev = dev_to_pxd_dev(dev);
	int suspend;

	suspend=pxd_suspend_state(pxd_dev);
	return sprintf(buf, "nfd:%d,suspend:%d,fpenabled:%d,fpactive:%d,app_suspend:%d\n",
			pxd_dev->fp.nfd, suspend, fastpath_enabled(pxd_dev), fastpath_active(pxd_dev),
			atomic_read(&pxd_dev->fp.app_suspend));
}

static ssize_t pxd_debug_store(struct device *dev,
			struct device_attribute *attr,
		        const char *buf, size_t count)
{
#ifdef __PX_FASTPATH__
	struct pxd_device *pxd_dev = dev_to_pxd_dev(dev);
	switch (buf[0]) {
	case 'Y': /* switch native path through IO failover */
		printk("dev:%llu - IO native path switch - IO failover\n", pxd_dev->dev_id);
		pxd_dev->fp.force_fail = true;
		break;
	case 'X': /* switch native path */
		printk("dev:%llu - IO native path switch - ctrl failover\n", pxd_dev->dev_id);
		pxd_debug_switch_nativepath(pxd_dev);
		break;
	case 's': /* suspend */
		printk("dev:%llu - IO suspend\n", pxd_dev->dev_id);
		pxd_suspend_io(pxd_dev);
		break;
	case 'r': /* resume */
		printk("dev:%llu - IO resume\n", pxd_dev->dev_id);
		pxd_resume_io(pxd_dev);
		break;
	case 'x': /* switch fastpath*/
		printk("dev:%llu - IO fast path switch\n", pxd_dev->dev_id);
		pxd_debug_switch_fastpath(pxd_dev);
		break;
	case 'S': /* app suspend */
		printk("dev:%llu - requesting IO suspend\n", pxd_dev->dev_id);
		pxd_request_suspend(pxd_dev, false, true);
		break;
	case 'R': /* app resume */
		printk("dev:%llu - requesting IO resume\n", pxd_dev->dev_id);
		pxd_request_resume(pxd_dev);
		break;
	default:
		/* no action */
		printk("dev:%llu - no action for %c\n", pxd_dev->dev_id, buf[0]);
	}
#endif
	return count;
}

static ssize_t pxd_inprogress_show(struct device *dev,
                     struct device_attribute *attr, char *buf)
{
	struct pxd_device *pxd_dev = dev_to_pxd_dev(dev);

	return sprintf(buf, "%d", atomic_read(&pxd_dev->ncount));
}

static int pxd_nodewipe_cleanup(struct pxd_context *ctx)
{
	struct list_head *cur;

	if (READ_ONCE(ctx->fc.connected)) {
		return -EINVAL;
	}

	if (ctx->num_devices == 0) {
		return 0;
	}

	spin_lock(&ctx->lock);
	list_for_each(cur, &ctx->list) {
		struct pxd_device *pxd_dev = container_of(cur, struct pxd_device, node);

		pxd_fastpath_reset_device(pxd_dev);
	}
	spin_unlock(&ctx->lock);

	return 0;
}

static ssize_t pxd_release_store(struct device *dev,
			struct device_attribute *attr, const char *buf, size_t count)
{
	static const char wipemagic[] = "P0RXR3l3@53";
	int i;
	struct pxd_context *ctx;

	if (!strncmp(wipemagic, buf, sizeof(wipemagic))) {
		printk("pxd kernel node wipe action initiated\n");
		for (i = 0; i < pxd_num_contexts; ++i) {
			ctx = &pxd_contexts[i];
			if (READ_ONCE(ctx->fc.connected)) {
				printk("%s px is still connected... cannot release\n", __func__);
				break;
			}
			if (ctx->num_devices == 0) {
				continue;
			}

			pxd_nodewipe_cleanup(ctx);
		}
	}

	return count;
}

static DEVICE_ATTR(size, S_IRUGO, pxd_size_show, NULL);
static DEVICE_ATTR(major, S_IRUGO, pxd_major_show, NULL);
static DEVICE_ATTR(minor, S_IRUGO, pxd_minor_show, NULL);
static DEVICE_ATTR(timeout, S_IRUGO|S_IWUSR, pxd_timeout_show, pxd_timeout_store);
static DEVICE_ATTR(active, S_IRUGO, pxd_active_show, NULL);
static DEVICE_ATTR(congested, S_IRUGO|S_IWUSR, pxd_congestion_show, pxd_congestion_set);
static DEVICE_ATTR(fastpath, S_IRUGO|S_IWUSR, pxd_fastpath_state, pxd_fastpath_update);
static DEVICE_ATTR(mode, S_IRUGO, pxd_mode_show, NULL);
static DEVICE_ATTR(debug, S_IRUGO|S_IWUSR, pxd_debug_show, pxd_debug_store);
static DEVICE_ATTR(inprogress, S_IRUGO, pxd_inprogress_show, NULL);
static DEVICE_ATTR(release, S_IWUSR, NULL, pxd_release_store);

static struct attribute *pxd_attrs[] = {
	&dev_attr_size.attr,
	&dev_attr_major.attr,
	&dev_attr_minor.attr,
	&dev_attr_timeout.attr,
	&dev_attr_active.attr,
	&dev_attr_congested.attr,
	&dev_attr_fastpath.attr,
	&dev_attr_mode.attr,
	&dev_attr_debug.attr,
	&dev_attr_inprogress.attr,
	&dev_attr_release.attr,
	NULL
};

static struct attribute_group pxd_attr_group = {
	.attrs = pxd_attrs,
};

static const struct attribute_group *pxd_attr_groups[] = {
	&pxd_attr_group,
	NULL
};

static void pxd_sysfs_dev_release(struct device *dev)
{
}

static struct device_type pxd_device_type = {
	.name		= "pxd",
	.groups		= pxd_attr_groups,
	.release	= pxd_sysfs_dev_release,
};

static void pxd_dev_device_release(struct device *dev)
{
	struct pxd_device *pxd_dev = dev_to_pxd_dev(dev);

	pxd_free_disk(pxd_dev);
	ida_simple_remove(&pxd_minor_ida, pxd_dev->minor);
	pxd_mem_printk("freeing dev %llu pxd device %px\n", pxd_dev->dev_id, pxd_dev);
	pxd_dev->magic = PXD_POISON;
	kfree(pxd_dev);
}

static int pxd_bus_add_dev(struct pxd_device *pxd_dev)
{
	struct device *dev;
	int ret;

	dev = &pxd_dev->dev;
	dev->bus = &pxd_bus_type;
	dev->type = &pxd_device_type;
	dev->parent = &pxd_root_dev;
	dev->release = pxd_dev_device_release;
	dev_set_name(dev, "%d", pxd_dev->minor);
	ret = device_register(dev);

	return ret;
}

static int pxd_sysfs_init(void)
{
	int err;

	err = device_register(&pxd_root_dev);
	if (err < 0)
		return err;

	err = bus_register(&pxd_bus_type);
	if (err < 0)
		device_unregister(&pxd_root_dev);

	return err;
}

static void pxd_sysfs_exit(void)
{
	bus_unregister(&pxd_bus_type);
	device_unregister(&pxd_root_dev);
}

static int pxd_control_open(struct inode *inode, struct file *file)
{
	struct pxd_context *ctx;
	struct fuse_conn *fc;

	if (strcmp(current->comm, PROC_PX_STORAGE) != 0 &&
		strcmp(current->comm, PROC_PX_CONTROL) != 0 &&
		strcmp(current->comm, PROC_PX_UT) != 0 &&
		strcmp(current->comm, PROC_PX_TOOL) != 0) {
		printk_ratelimited(KERN_INFO "%s: invalid access comm=%s",
			__func__, current->comm);
		return -EACCES;
	}
	if (!((uintptr_t)pxd_contexts <= (uintptr_t)file->f_op &&
		(uintptr_t)file->f_op < (uintptr_t)(pxd_contexts + pxd_num_contexts))) {
		printk(KERN_ERR "%s: invalid fops struct\n", __func__);
		return -EINVAL;
	}

	ctx = container_of(file->f_op, struct pxd_context, fops);
	if (ctx->id >= pxd_num_contexts_exported) {
		return 0;
	}

	fc = &ctx->fc;
	if (READ_ONCE(fc->connected) == 1) {
		printk(KERN_ERR "%s: pxd-control-%d(%lld) already open\n", __func__,
			ctx->id, ctx->open_seq);
		return -EINVAL;
	}

	// abort work cannot be active while restarting requests
	cancel_delayed_work_sync(&ctx->abort_work);
	fuse_restart_requests(fc);

	spin_lock(&ctx->lock);
	pxd_timeout_secs = PXD_TIMER_SECS_DEFAULT;
	WRITE_ONCE(fc->connected, 1);
	spin_unlock(&ctx->lock);

	WRITE_ONCE(fc->allow_disconnected, 1);
	file->private_data = fc;

	pxdctx_set_connected(ctx, true);

	++ctx->open_seq;

	printk(KERN_INFO "%s: pxd-control-%d(%lld) open OK\n", __func__, ctx->id,
		ctx->open_seq);

	return 0;
}

/** Note that this will not be called if userspace doesn't cleanup. */
static int pxd_control_release(struct inode *inode, struct file *file)
{
	struct pxd_context *ctx;
	ctx = container_of(file->f_op, struct pxd_context, fops);
	if (ctx->id >= pxd_num_contexts_exported) {
		return 0;
	}

	spin_lock(&ctx->lock);
	if (READ_ONCE(ctx->fc.connected) == 0) {
		pxd_printk("%s: not opened\n", __func__);
	} else {
		WRITE_ONCE(ctx->fc.connected, 0);
	}

	schedule_delayed_work(&ctx->abort_work, pxd_timeout_secs * HZ);
	spin_unlock(&ctx->lock);

	printk(KERN_INFO "%s: pxd-control-%d(%lld) close OK\n", __func__, ctx->id,
		ctx->open_seq);
	return 0;
}

static struct miscdevice pxd_miscdev = {
	.minor		= MISC_DYNAMIC_MINOR,
	.name		= "pxd/pxd-control",
};

MODULE_ALIAS("devname:pxd-control");

static void pxd_fuse_conn_release(struct fuse_conn *conn)
{
}

static void pxd_abort_context(struct work_struct *work)
{
	struct pxd_context *ctx = container_of(to_delayed_work(work), struct pxd_context,
		abort_work);
	struct fuse_conn *fc = &ctx->fc;

	BUG_ON(READ_ONCE(fc->connected));

	printk(KERN_ERR "PXD_TIMEOUT (%s:%u): Aborting all requests...",
		ctx->name, ctx->id);

	WRITE_ONCE(fc->allow_disconnected, 0);

	/* Let other threads see the value of allow_disconnected. */
	synchronize_rcu();

	spin_lock(&fc->lock);
	fuse_end_queued_requests(fc);
	spin_unlock(&fc->lock);
	pxdctx_set_connected(ctx, false);
}

static int pxd_context_init(struct pxd_context *ctx, int i)
{
	int err;

	spin_lock_init(&ctx->lock);
	ctx->id = i;
	ctx->open_seq = 0;
	ctx->fops = fuse_dev_operations;
	ctx->fops.owner = THIS_MODULE;
	ctx->fops.open = pxd_control_open;
	ctx->fops.release = pxd_control_release;
	ctx->fops.unlocked_ioctl = pxd_control_ioctl;

	if (ctx->id < pxd_num_contexts_exported) {
		err = fuse_conn_init(&ctx->fc);
		if (err)
			return err;
	}

	ctx->fc.release = pxd_fuse_conn_release;
	ctx->fc.allow_disconnected = 1;
	INIT_LIST_HEAD(&ctx->list);
	sprintf(ctx->name, "pxd/pxd-control-%d", i);
	ctx->miscdev.minor = MISC_DYNAMIC_MINOR;
	ctx->miscdev.name = ctx->name;
	ctx->miscdev.fops = &ctx->fops;
	INIT_DELAYED_WORK(&ctx->abort_work, pxd_abort_context);
	return 0;
}

static void pxd_context_destroy(struct pxd_context *ctx)
{
	misc_deregister(&ctx->miscdev);
	cancel_delayed_work_sync(&ctx->abort_work);
	if (ctx->id < pxd_num_contexts_exported) {
		fuse_abort_conn(&ctx->fc);
		fuse_conn_put(&ctx->fc);
	}
}

static int pxd_init(void)
{
	int err, i, j;

	err = fuse_dev_init();
	if (err) {
		printk(KERN_ERR "pxd: failed to initialize fuse: %d\n", err);
		goto out;
	}

	pxd_contexts = kzalloc(sizeof(pxd_contexts[0]) * pxd_num_contexts,
		GFP_KERNEL);
	err = -ENOMEM;
	if (!pxd_contexts) {
		printk(KERN_ERR "pxd: failed to allocate memory\n");
		goto out_fuse_dev;
	}

	for (i = 0; i < pxd_num_contexts; ++i) {
		struct pxd_context *ctx = &pxd_contexts[i];
		err = pxd_context_init(ctx, i);
		if (err) {
			printk(KERN_ERR "pxd: failed to initialize connection\n");
			goto out_fuse;
		}

		err = misc_register(&ctx->miscdev);
		if (err) {
			printk(KERN_ERR "pxd: failed to register dev %s %d: %d\n",
				ctx->miscdev.name, i, err);
			goto out_fuse;
		}
	}

	pxd_miscdev.fops = &pxd_contexts[0].fops;
	err = misc_register(&pxd_miscdev);
	if (err) {
		printk(KERN_ERR "pxd: failed to register dev %s: %d\n",
			pxd_miscdev.name, err);
		goto out_fuse;
	}
	pxd_major = register_blkdev(0, "pxd");
	if (pxd_major < 0) {
		err = pxd_major;
		printk(KERN_ERR "pxd: failed to register dev pxd: %d\n", err);
		goto out_misc;
	}

	err = pxd_sysfs_init();
	if (err) {
		printk(KERN_ERR "pxd: failed to initialize sysfs: %d\n", err);
		goto out_blkdev;
	}

	err = fastpath_init();
	if (err) {
		printk(KERN_ERR "pxd: fastpath initialization failed: %d\n", err);
		goto out_blkdev;
	}
#ifdef __PX_BLKMQ__
	printk(KERN_INFO "pxd: blk-mq driver loaded version %s, features %#x\n",
			gitversion, pxd_supported_features());
#else
	printk(KERN_INFO "pxd: driver loaded version %s, features %#x\n",
			gitversion, pxd_supported_features());
#endif

	return 0;

out_blkdev:
	unregister_blkdev(0, "pxd");
out_misc:
	misc_deregister(&pxd_miscdev);
out_fuse:
	for (j = 0; j < i; ++j) {
		pxd_context_destroy(&pxd_contexts[j]);
	}
	kfree(pxd_contexts);
out_fuse_dev:
	fuse_dev_cleanup();
out:
	return err;
}

static void pxd_exit(void)
{
	int i;

	fastpath_cleanup();
	pxd_sysfs_exit();
	unregister_blkdev(pxd_major, "pxd");
	misc_deregister(&pxd_miscdev);

	for (i = 0; i < pxd_num_contexts; ++i) {
		/* force cleanup @@@ */
		WRITE_ONCE(pxd_contexts[i].fc.connected, 1);
		pxd_context_destroy(&pxd_contexts[i]);
	}

	fuse_dev_cleanup();

	kfree(pxd_contexts);

	printk(KERN_INFO "pxd: driver unloaded\n");
}

module_init(pxd_init);
module_exit(pxd_exit);

MODULE_LICENSE("GPL");
MODULE_VERSION(VERTOSTR(PXD_VERSION));
