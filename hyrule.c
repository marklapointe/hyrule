/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2026, Mark LaPointe <mark@cloudbsd.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "hyrule.h"
#include <sys/module.h>
#include <sys/kernel.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/taskqueue.h>
#include <sys/syscallsubr.h>
#include <sys/linker.h>

/*
 * Hyrule Kernel Module Core
 */

struct prop_head prop_list = LIST_HEAD_INITIALIZER(prop_list);

/* Global locks */
struct mtx hyrule_mtx;	/* Protects prop_list */
struct sx hyrule_sx;	/* Protects property values across uiomove */

static struct timeout_task *hyrule_death_task;
static int hyrule_unloading = 0;

static void
hyrule_death_worker(void *arg, int pending)
{
	int error;
	struct timeout_task *tt = arg;

	/*
	 * __this_linker_file is a special symbol provided by the kernel linker
	 * that points to the struct linker_file for the current module.
	 */
	if (__this_linker_file != NULL) {
		printf("[HYRULE] It's dangerous to go alone! Link is gone. Unloading module...\n");
		hyrule_unloading = 1;

		/*
		 * kern_kldunload() is the kernel-side implementation of the kldunload syscall.
		 * It safely triggers the module's MOD_UNLOAD event.
		 * 
		 * Note: After this call succeeds, the module's code and data are unmapped.
		 * We rely on the fact that the task structure was heap-allocated to avoid
		 * a panic in the taskqueue loop when it accesses the task structure
		 * after we return.
		 */
		error = kern_kldunload(curthread, __this_linker_file->id, LINKER_UNLOAD_NORMAL);
		if (error != 0) {
			printf("[HYRULE] Self-unload failed (error=%d). Retrying in 1s...\n", error);
			hyrule_unloading = 0;
			taskqueue_enqueue_timeout(taskqueue_thread, tt, hz);
		}
	} else {
		printf("[HYRULE] Could not find linker file for self-unload.\n");
	}
}

static struct cdevsw hyrule_cdevsw = {
	.d_version = D_VERSION,
	.d_open = hyrule_open,
	.d_close = hyrule_close,
	.d_read = hyrule_read,
	.d_write = hyrule_write,
	.d_ioctl = hyrule_ioctl,
	.d_poll = hyrule_poll,
	.d_mmap = hyrule_mmap,
	.d_strategy = hyrule_strategy,
	.d_kqfilter = hyrule_kqfilter,
	.d_fdopen = hyrule_fdopen,
	.d_mmap_single = hyrule_mmap_single,
	.d_purge = hyrule_purge,
	.d_name = "hyrule",
};

int
hyrule_open(struct cdev *dev, int oflags, int devtype, struct thread *td)
{
	return (0);
}

int
hyrule_close(struct cdev *dev, int fflag, int devtype, struct thread *td)
{
	return (0);
}

int
hyrule_ioctl(struct cdev *dev, u_long cmd, caddr_t data, int fflag, struct thread *td)
{
	/*
	 * ioctl (Input/Output Control) is used for device-specific operations
	 * that don't fit into the standard read/write model.
	 * Examples: Ejecting a CD-ROM, setting serial port speed, or
	 * getting the Master Sword's power level.
	 */
	return (ENOTTY); /* Traditional error for unsupported ioctls */
}

int
hyrule_poll(struct cdev *dev, int events, struct thread *td)
{
	/*
	 * poll is used for asynchronous I/O multiplexing (select/poll/epoll).
	 * It allows a user program to wait for multiple devices to become
	 * ready for reading or writing without blocking on each one.
	 * Useful for things like network sockets or keyboard input.
	 */
	return (events & (POLLIN | POLLRDNORM | POLLOUT | POLLWRNORM));
}

int
hyrule_mmap(struct cdev *dev, vm_ooffset_t offset, vm_paddr_t *paddr, int nprot, vm_memattr_t *memattr)
{
	/*
	 * mmap allows mapping device memory directly into a process's address space.
	 * This is extremely useful for high-performance graphics (framebuffers)
	 * or shared memory between kernel and userland.
	 * Not very useful for simple character stats, hence we return EINVAL.
	 */
	return (EINVAL);
}

int
hyrule_kqfilter(struct cdev *dev, struct knote *kn)
{
	/*
	 * kqfilter is used for kqueue, FreeBSD's scalable event notification mechanism.
	 * It's a more modern and efficient alternative to poll/select.
	 */
	return (EINVAL);
}

void
hyrule_strategy(struct bio *bp)
{
	/*
	 * strategy is the primary entry point for block I/O. It handles bio structures
	 * representing read or write requests to a disk or similar device.
	 * For character devices, it's rarely used directly unless wrapping a block device.
	 */
	biofinish(bp, NULL, ENODEV);
}

int
hyrule_fdopen(struct cdev *dev, int oflags, struct thread *td, struct file *fp)
{
	/*
	 * fdopen is called when a file descriptor is created for the device.
	 * Most drivers can just rely on d_open.
	 */
	return (0);
}

int
hyrule_mmap_single(struct cdev *cdev, vm_ooffset_t *offset, vm_size_t size, struct vm_object **object, int nprot)
{
	/*
	 * mmap_single is a specialized variant of mmap for mapping a single object.
	 */
	return (ENODEV);
}

void
hyrule_purge(struct cdev *dev)
{
	/*
	 * purge is used to invalidate any cached data for the device.
	 */
}

int
hyrule_read(struct cdev *dev, struct uio *uio, int ioflag)
{
	struct hyrule_prop *p = dev->si_drv1;
	int error;
	size_t len;

	if (p == NULL)
		return (ENXIO);

	sx_slock(&hyrule_sx);
	len = strnlen(p->value, sizeof(p->value));
	if (uio->uio_offset >= len) {
		sx_sunlock(&hyrule_sx);
		return (0);
	}
	error = uiomove(p->value + uio->uio_offset, len - uio->uio_offset, uio);
	sx_sunlock(&hyrule_sx);
	return (error);
}

int
hyrule_write(struct cdev *dev, struct uio *uio, int ioflag)
{
	struct hyrule_prop *p = dev->si_drv1;
	int error;
	size_t len;

	if (p == NULL)
		return (ENXIO);

	sx_xlock(&hyrule_sx);
	if (uio->uio_offset >= sizeof(p->value) - 1) {
		sx_xunlock(&hyrule_sx);
		return (EFBIG);
	}
	len = MIN(uio->uio_resid, sizeof(p->value) - 1 - uio->uio_offset);
	error = uiomove(p->value + uio->uio_offset, len, uio);
	p->value[sizeof(p->value) - 1] = '\0';

	/* Check for Link's death */
	if (error == 0 && strcmp(p->name, "characters/link/stats/health") == 0) {
		long hp = strtol(p->value, NULL, 10);
		if (hp <= 0) {
			printf("[HYRULE] Link has no life left! GAME OVER.\n");
			if (hyrule_death_task != NULL)
				taskqueue_enqueue_timeout(taskqueue_thread, hyrule_death_task, hz);
		}
	}

	sx_xunlock(&hyrule_sx);
	return (error);
}

int
add_hyrule_node_custom(const char *path, const char *initial_val, struct cdevsw *sw)
{
	struct hyrule_prop *p;
	struct make_dev_args args;
	int error;

	if (path == NULL || initial_val == NULL)
		return (EINVAL);

	p = malloc(sizeof(*p), M_DEVBUF, M_WAITOK | M_ZERO);
	if (p == NULL)
		return (ENOMEM);

	strlcpy(p->name, path, sizeof(p->name));
	strlcpy(p->value, initial_val, sizeof(p->value));

	make_dev_args_init(&args);
	args.mda_flags = MAKEDEV_CHECKNAME | MAKEDEV_WAITOK;
	args.mda_devsw = sw;
	args.mda_uid = UID_ROOT;
	args.mda_gid = GID_WHEEL;
	args.mda_mode = 0644;
	args.mda_si_drv1 = p;

	error = make_dev_s(&args, &p->cdev, "hyrule/%s", p->name);
	if (error != 0) {
		printf("[HYRULE] Failed to create /dev/hyrule/%s (error=%d)\n", p->name, error);
		free(p, M_DEVBUF);
		return (error);
	}

	mtx_lock(&hyrule_mtx);
	LIST_INSERT_HEAD(&prop_list, p, next);
	mtx_unlock(&hyrule_mtx);
	return (0);
}

int
add_hyrule_node(const char *path, const char *initial_val)
{
	return add_hyrule_node_custom(path, initial_val, &hyrule_cdevsw);
}

static int
hyrule_loader(struct module *mod, int cmd, void *arg)
{
	int error = 0;
	struct hyrule_prop *p;

	switch (cmd) {
	case MOD_LOAD:
		mtx_init(&hyrule_mtx, "hyrule list lock", NULL, MTX_DEF);
		sx_init(&hyrule_sx, "hyrule data lock");
		
		/* 
		 * Heap-allocate the death task to prevent use-after-free
		 * during self-unload. If the module unloads itself,
		 * the taskqueue may still access this structure.
		 */
		hyrule_death_task = malloc(sizeof(*hyrule_death_task), M_DEVBUF, M_WAITOK | M_ZERO);
		TIMEOUT_TASK_INIT(taskqueue_thread, hyrule_death_task, 0, hyrule_death_worker, hyrule_death_task);
		
		hyrule_map_init();

		/* Help device */
		error = add_hyrule_node("help", 
		    "Welcome to the Hyrule Kernel Module!\n\n"
		    "Map display: cat /dev/hyrule/map\n"
		    "World config: /dev/hyrule/world/map_config\n"
		    "Move Link: echo 'up' > /dev/hyrule/characters/link/location/move\n"
		    "Example: echo 'e' > /dev/hyrule/characters/link/location/move (to move east)\n\n"
		    "Be careful, it's dangerous to go alone!\n");
		if (error) goto fail;

		/* Map Devices */
		error = add_hyrule_node_custom("map", "", &hyrule_map_cdevsw);
		if (error) goto fail;
		error = add_hyrule_node_custom("world/map_config", "", &hyrule_map_config_cdevsw);
		if (error) goto fail;
		error = add_hyrule_node_custom("characters/link/location/move", "", &hyrule_move_cdevsw);
		if (error) goto fail;

		/* Characters */
		error = add_hyrule_node("characters/link/stats/health", "100\n");
		if (error) goto fail;
		error = add_hyrule_node("characters/link/stats/stamina", "100\n");
		if (error) goto fail;
		error = add_hyrule_node("characters/link/stats/rupees", "0\n");
		if (error) goto fail;
		error = add_hyrule_node("characters/link/weapons/sword", "Master Sword\n");
		if (error) goto fail;
		error = add_hyrule_node("characters/link/weapons/bow", "Hero's Bow\n");
		if (error) goto fail;

		/* Zelda */
		error = add_hyrule_node("characters/zelda/stats/health", "100\n");
		if (error) goto fail;
		error = add_hyrule_node("characters/zelda/stats/magic", "100\n");
		if (error) goto fail;

		/* Ganon */
		error = add_hyrule_node("characters/ganon/stats/health", "200\n");
		if (error) goto fail;
		error = add_hyrule_node("characters/ganon/status/condition", "ALIVE\n");
		if (error) goto fail;

		/* Objects */
		error = add_hyrule_node("objects/triforce/parts/courage", "Link\n");
		if (error) goto fail;

		printf("[HYRULE] Hyrule is now mapped to /dev/hyrule/\n");
		break;

	case MOD_UNLOAD:
		if (!hyrule_unloading) {
			if (hyrule_death_task != NULL) {
				taskqueue_cancel_timeout(taskqueue_thread, hyrule_death_task, NULL);
				taskqueue_drain_timeout(taskqueue_thread, hyrule_death_task);
				free(hyrule_death_task, M_DEVBUF);
				hyrule_death_task = NULL;
			}
		}

		while (1) {
			mtx_lock(&hyrule_mtx);
			p = LIST_FIRST(&prop_list);
			if (p == NULL) {
				mtx_unlock(&hyrule_mtx);
				break;
			}
			LIST_REMOVE(p, next);
			mtx_unlock(&hyrule_mtx);
			destroy_dev(p->cdev);
			free(p, M_DEVBUF);
		}
		mtx_destroy(&hyrule_mtx);
		sx_destroy(&hyrule_sx);
		printf("[HYRULE] Link has saved the game. Leaving the kernel...\n");
		break;

	case MOD_SHUTDOWN:
		/*
		 * Called during system shutdown. Useful for ensuring hardware
		 * is in a safe state or flushing caches to disk.
		 */
		printf("[HYRULE] Hyrule is fading as the system shuts down...\n");
		break;

	case MOD_QUIESCE:
		/*
		 * Called before unloading to see if the module is ready to be
		 * unloaded. Returning a non-zero error will prevent the unload.
		 */
		printf("[HYRULE] Checking if Link is ready to leave...\n");
		error = 0;
		break;

	default:
		error = EOPNOTSUPP;
		break;
	}
	return (error);

fail:
	if (hyrule_death_task != NULL) {
		taskqueue_cancel_timeout(taskqueue_thread, hyrule_death_task, NULL);
		taskqueue_drain_timeout(taskqueue_thread, hyrule_death_task);
		free(hyrule_death_task, M_DEVBUF);
		hyrule_death_task = NULL;
	}

	while (1) {
		mtx_lock(&hyrule_mtx);
		p = LIST_FIRST(&prop_list);
		if (p == NULL) {
			mtx_unlock(&hyrule_mtx);
			break;
		}
		LIST_REMOVE(p, next);
		mtx_unlock(&hyrule_mtx);
		if (p->cdev)
			destroy_dev(p->cdev);
		free(p, M_DEVBUF);
	}
	mtx_destroy(&hyrule_mtx);
	sx_destroy(&hyrule_sx);
	return (error);
}

static moduledata_t hyrule_mod = {
	"hyrule",
	hyrule_loader,
	NULL
};

DECLARE_MODULE(hyrule, hyrule_mod, SI_SUB_DRIVERS, SI_ORDER_MIDDLE);
