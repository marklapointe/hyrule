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
#include <sys/sysctl.h>
#include <sys/smp.h>
#include <sys/resource.h>
#include <sys/linker.h>
#include <sys/kthread.h>
#include <sys/proc.h>
#include <sys/syscallsubr.h>
#include <sys/sbuf.h>

/*
 * Hyrule Kernel Module Core
 *
 * This module implements a hierarchical property system represented as
 * character devices under /dev/hyrule/. It demonstrates several FreeBSD
 * kernel programming concepts:
 * - Character device drivers (cdev)
 * - Locking (mtx and sx)
 * - Kernel threads (kproc) and taskqueues
 * - Cross-module interaction (kern_kldload, sysctl)
 * - Safe string/buffer management (sbuf)
 * - File systems and UIO (User I/O)
 */

/* 
 * Global list of all properties (nodes) created by the module.
 * Each node corresponds to a file in /dev/hyrule/.
 */
struct prop_head prop_list = LIST_HEAD_INITIALIZER(prop_list);

/* 
 * Global locks:
 * 
 * hyrule_mtx: A leaf mutex (MTX_DEF). Mutexes are fast, non-sleepable locks.
 * We use it to protect the integrity of the 'prop_list' linked list itself
 * during insertions, removals, and iterations. Because it's a mutex, we
 * cannot perform any operation that might sleep (like I/O or certain mallocs)
 * while holding it.
 *
 * hyrule_sx: A shared/exclusive (SX) lock. SX locks are sleepable and support
 * multiple concurrent readers (sx_slock) but only one writer (sx_xlock).
 * We use this to protect the 'value' strings within properties. Since moving
 * data to/from userland (uiomove) can sleep (e.g., if the user's page is swapped out),
 * we must use a sleepable lock like SX instead of a mutex.
 */
struct mtx hyrule_mtx;	/* Protects prop_list structural integrity */
struct sx hyrule_sx;	/* Protects property 'value' contents across sleepable I/O */

/*
 * Status tracking for optional temperature modules.
 * These are updated by a background kproc during module initialization.
 */
static int hyrule_coretemp_res = -1; /* -1: not attempted, 0/EEXIST: success, errno: error */
static int hyrule_amdtemp_res = -1;

/* 
 * Prototypes for device switch handlers.
 * cdevsw (Character Device Switch) is the "vtable" for character devices.
 */
static struct cdevsw hyrule_cdevsw;
static d_read_t hyrule_cpu_read;
static void hyrule_update_status_nodes_task(void *context, int pending);

/*
 * Specialized cdevsw for the CPU stats node.
 * It uses a custom read handler to generate JSON on-the-fly.
 */
static struct cdevsw hyrule_cpu_cdevsw = {
	.d_version = D_VERSION,
	.d_open = hyrule_open,
	.d_close = hyrule_close,
	.d_read = hyrule_cpu_read,
	.d_name = "hyrule_cpu",
};

/* Global game state variables */
int hyrule_power = 1;      /* System power state (0=off, 1=on) */
int hyrule_cartridge = 0;  /* Cartridge cleanliness (0=dusty, 1=clean) */
int hyrule_invincible = 0; /* Cheat mode active? */

/**
 * hyrule_get_prop_int - Safely retrieve an integer value from a property node.
 * @name: The path of the property (e.g., "characters/link/stats/health")
 * @default_val: Value to return if the property is not found.
 *
 * This function iterates through the global property list while holding
 * the list mutex. It converts the string value to a long.
 */
int
hyrule_get_prop_int(const char *name, int default_val)
{
	struct hyrule_prop *p;
	int val = default_val;

	mtx_lock(&hyrule_mtx);
	LIST_FOREACH(p, &prop_list, next) {
		if (strcmp(p->name, name) == 0) {
			val = strtol(p->value, NULL, 10);
			break;
		}
	}
	mtx_unlock(&hyrule_mtx);
	return (val);
}

/**
 * hyrule_set_prop_int - Safely update a property node with an integer value.
 * @name: The path of the property.
 * @val: The integer value to set.
 *
 * Formats the integer as a string and stores it in the property's value buffer.
 */
void
hyrule_set_prop_int(const char *name, int val)
{
	struct hyrule_prop *p;

	mtx_lock(&hyrule_mtx);
	LIST_FOREACH(p, &prop_list, next) {
		if (strcmp(p->name, name) == 0) {
			snprintf(p->value, sizeof(p->value), "%d\n", val);
			break;
		}
	}
	mtx_unlock(&hyrule_mtx);
}

/**
 * hyrule_is_active - Check if the game is in a playable state.
 *
 * The game is active if power is on AND Link has health > 0 (or is invincible).
 */
int
hyrule_is_active(void)
{
	struct hyrule_prop *p;
	long hp = 1;

	if (hyrule_power == 0)
		return (0);

	if (hyrule_invincible)
		return (1);

	mtx_lock(&hyrule_mtx);
	LIST_FOREACH(p, &prop_list, next) {
		if (strcmp(p->name, "characters/link/stats/health") == 0) {
			hp = strtol(p->value, NULL, 10);
		}
	}
	mtx_unlock(&hyrule_mtx);

	return (hp > 0);
}

/**
 * hyrule_reset - Restore all properties to their default values.
 *
 * Used when the system is powered on, reset, or Link dies.
 */
void
hyrule_reset(void)
{
	struct hyrule_prop *p;

	/*
	 * We don't lock hyrule_sx here because this might be called from
	 * hyrule_write which already holds it.
	 * But we should ensure we have exclusive access if called from elsewhere.
	 * For now, we only call it from hyrule_write or during init.
	 */
	mtx_lock(&hyrule_mtx);
	LIST_FOREACH(p, &prop_list, next) {
		strlcpy(p->value, p->default_value, sizeof(p->value));
	}
	mtx_unlock(&hyrule_mtx);

	hyrule_invincible = 0;
	hyrule_update_status_nodes();
	hyrule_map_init();
	
	printf("[HYRULE] System Reset. Welcome back!\n");
}

/*
 * The main cdevsw for standard property nodes.
 * Most nodes under /dev/hyrule/ use these generic handlers.
 */
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

/**
 * hyrule_open - Called when a user opens a /dev/hyrule/ device.
 */
int
hyrule_open(struct cdev *dev, int oflags, int devtype, struct thread *td)
{
	return (0);
}

/**
 * hyrule_close - Called when the last file descriptor for the device is closed.
 */
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

/**
 * hyrule_read - Generic read handler for property nodes.
 * @dev: The character device being read.
 * @uio: The UIO structure describing the data transfer.
 * @ioflag: File status flags.
 *
 * This function handles 'cat' or other read operations on property nodes.
 * It uses the UIO (User I/O) subsystem, which is the standard way to transfer
 * data between kernel and user space in FreeBSD.
 * 
 * Key concept: uio_offset.
 * The kernel tracks the current read position in uio->uio_offset. If a user
 * reads 10 bytes, the next read will start at offset 10. We must respect this
 * to allow 'cat' and other tools to work correctly.
 */
int
hyrule_read(struct cdev *dev, struct uio *uio, int ioflag)
{
	struct hyrule_prop *p = dev->si_drv1;
	int error;
	size_t len;

	if (p == NULL)
		return (ENXIO);

	sx_slock(&hyrule_sx);

	/* Check if system is active */
	if (!hyrule_is_active() && 
	    strcmp(p->name, "console/power") != 0 && 
	    strcmp(p->name, "console/reset") != 0 &&
	    strcmp(p->name, "console/cartridge") != 0 &&
	    strcmp(p->name, "help") != 0) {
		const char *msg;
		if (hyrule_power == 0)
			msg = "POWER OFF\n";
		else
			msg = "GAME OVER\n";
		
		len = strlen(msg);
		if (uio->uio_offset >= len) {
			sx_sunlock(&hyrule_sx);
			return (0);
		}
		error = uiomove(__DECONST(char *, msg) + uio->uio_offset, len - uio->uio_offset, uio);
		sx_sunlock(&hyrule_sx);
		return (error);
	}

	len = strnlen(p->value, sizeof(p->value));
	if (uio->uio_offset >= len) {
		sx_sunlock(&hyrule_sx);
		return (0);
	}
	error = uiomove(p->value + uio->uio_offset, len - uio->uio_offset, uio);
	sx_sunlock(&hyrule_sx);
	return (error);
}

/**
 * hyrule_cpu_read - Specialized read handler for /dev/hyrule/console/cpu.
 *
 * This handler generates a dynamic JSON report containing:
 * - System CPU count (mp_ncpus)
 * - Per-CPU temperatures (via sysctl)
 * - Per-CPU execution ticks (via kern.cp_times)
 * - Status of the coretemp/amdtemp drivers.
 *
 * It uses the sbuf(9) API, which provides safe, auto-extending string buffers
 * in the kernel. This is much safer than manual snprintf into a fixed buffer.
 */
static int
hyrule_cpu_read(struct cdev *dev, struct uio *uio, int ioflag)
{
	struct sbuf *sb;
	long *times;
	int error;
	size_t len;
	size_t times_sz;

	/* 
	 * Use mp_ncpus to get the number of CPUs found by the kernel.
	 */
	times_sz = mp_ncpus * CPUSTATES * sizeof(long);
	times = malloc(times_sz, M_DEVBUF, M_WAITOK | M_ZERO);

	/* 
	 * Example of getting information from core kernel:
	 * We query kern.cp_times to get per-CPU tick counts.
	 */
	if (kernel_sysctlbyname(curthread, "kern.cp_times", times, &times_sz, NULL, 0, NULL, 0) != 0) {
		memset(times, 0, times_sz);
	}

	/* Use sbuf(9) for robust JSON generation */
	sb = sbuf_new_auto();
	if (sb == NULL) {
		free(times, M_DEVBUF);
		return (ENOMEM);
	}

	sbuf_printf(sb, "{\n");
	sbuf_printf(sb, "  \"ncpus\": %d,\n", mp_ncpus);
	
	/* Report status of temperature drivers */
	sbuf_printf(sb, "  \"drivers\": {\n");
	sbuf_printf(sb, "    \"coretemp\": \"%s\",\n", 
	    (hyrule_coretemp_res == 0 || hyrule_coretemp_res == EEXIST) ? "loaded" : 
	    (hyrule_coretemp_res == -1) ? "pending" : "not found");
	sbuf_printf(sb, "    \"amdtemp\": \"%s\"\n",
	    (hyrule_amdtemp_res == 0 || hyrule_amdtemp_res == EEXIST) ? "loaded" :
	    (hyrule_amdtemp_res == -1) ? "pending" : "not found");
	sbuf_printf(sb, "  },\n");

	sbuf_printf(sb, "  \"cpus\": [\n");

	for (int i = 0; i < mp_ncpus; i++) {
		int temp = 0;
		size_t temp_sz = sizeof(temp);
		char name[64];

		/* 
		 * Example of getting information from other modules:
		 * We query coretemp or amdtemp via their sysctl interfaces.
		 */
		snprintf(name, sizeof(name), "dev.cpu.%d.temperature", i);
		error = kernel_sysctlbyname(curthread, name, &temp, &temp_sz, NULL, 0, NULL, 0);

		sbuf_printf(sb, "    {\n      \"id\": %d,\n", i);
		if (error == 0) {
			/* Convert deci-Kelvin to Celsius */
			int val = temp - 2731;
			int whole = val / 10;
			int frac = val % 10;
			if (frac < 0) frac = -frac;
			sbuf_printf(sb, "      \"temperature_c\": %d.%d,\n", whole, frac);
		} else {
			sbuf_printf(sb, "      \"temperature_c\": null,\n");
			sbuf_printf(sb, "      \"temperature_status\": \"%s\",\n",
			    (error == ENOENT) ? "no sysctl" : "error");
		}

		/* Use the tick counts we retrieved earlier */
		sbuf_printf(sb, "      \"stats\": {\n");
		sbuf_printf(sb, "        \"user\": %ld,\n", times[i * CPUSTATES + CP_USER]);
		sbuf_printf(sb, "        \"nice\": %ld,\n", times[i * CPUSTATES + CP_NICE]);
		sbuf_printf(sb, "        \"sys\": %ld,\n", times[i * CPUSTATES + CP_SYS]);
		sbuf_printf(sb, "        \"intr\": %ld,\n", times[i * CPUSTATES + CP_INTR]);
		sbuf_printf(sb, "        \"idle\": %ld\n", times[i * CPUSTATES + CP_IDLE]);
		sbuf_printf(sb, "      }\n");
		
		sbuf_printf(sb, "    }%s\n", (i == mp_ncpus - 1) ? "" : ",");
	}

	sbuf_printf(sb, "  ]\n}\n");
	sbuf_finish(sb);

	len = sbuf_len(sb);
	if (uio->uio_offset >= len) {
		error = 0;
		goto out;
	}

	error = uiomove(sbuf_data(sb) + uio->uio_offset, len - uio->uio_offset, uio);

out:
	sbuf_delete(sb);
	free(times, M_DEVBUF);
	return (error);
}

/**
 * hyrule_update_prop_state - Process side-effects of property changes.
 * @p: The property that was just modified.
 * @loading: True if called during a game load operation.
 *
 * This function implements the "game logic":
 * - Turning power on/off (with the "dusty cartridge" gamble).
 * - Triggering a system reset.
 * - Cleaning/dirtying the cartridge.
 * - Checking for Game Over (Link's health).
 * - Toggling invincibility.
 */
static void
hyrule_update_prop_state(struct hyrule_prop *p, int loading)
{
	/* Handle Console commands */
	if (strcmp(p->name, "console/power") == 0) {
		int new_power = strtol(p->value, NULL, 10);
		if (new_power != hyrule_power) {
			if (new_power == 1 && !loading) {
				/* 80% chance to fail to turn on if dusty */
				if (hyrule_cartridge == 0 && (arc4random() % 100) < 80) {
					printf("[HYRULE] Power-on failed. Red light blinking. Cartridge dusty?\n");
					strlcpy(p->value, "0\n", sizeof(p->value));
					return;
				}
				hyrule_reset();
			}
			hyrule_power = new_power;
			printf("[HYRULE] Power set to %d\n", hyrule_power);
			if (hyrule_power == 0 && !loading) {
				/* 50% chance to become dusty when turned off (the gamble) */
				if ((arc4random() % 100) < 50) {
					hyrule_cartridge = 0;
					struct hyrule_prop *cp;
					mtx_lock(&hyrule_mtx);
					LIST_FOREACH(cp, &prop_list, next) {
						if (strcmp(cp->name, "console/cartridge") == 0) {
							strlcpy(cp->value, "dusty\n", sizeof(cp->value));
							break;
						}
					}
					mtx_unlock(&hyrule_mtx);
					printf("[HYRULE] Cartridge is now dusty after power cycle.\n");
				}
			}
		}
	} else if (strcmp(p->name, "console/reset") == 0) {
		if (strtol(p->value, NULL, 10) == 1) {
			hyrule_reset();
			strlcpy(p->value, "0\n", sizeof(p->value));
		}
	} else if (strcmp(p->name, "console/cartridge") == 0) {
		if (strncmp(p->value, "blow", 4) == 0 || strncmp(p->value, "clean", 5) == 0) {
			hyrule_cartridge = 1;
			if (strncmp(p->value, "blow", 4) == 0)
				strlcpy(p->value, "clean\n", sizeof(p->value));
			printf("[HYRULE] Cartridge is now clean and ready to play!\n");
		} else {
			/* Any other write makes it dusty again */
			hyrule_cartridge = 0;
			strlcpy(p->value, "dusty\n", sizeof(p->value));
		}
	} else if (strcmp(p->name, "characters/link/stats/health") == 0) {
		long hp = strtol(p->value, NULL, 10);
		if (hp <= 0) {
			printf("[HYRULE] Link has no life left! GAME OVER.\n");
		}
	} else if (strcmp(p->name, "characters/link/status/invincible") == 0) {
		int new_invincible = strtol(p->value, NULL, 10);
		if (new_invincible != hyrule_invincible) {
			hyrule_invincible = new_invincible;
			hyrule_update_status_nodes();
		}
	}
}

static struct task status_update_task;
static struct hyrule_prop *invincible_node = NULL;

/**
 * hyrule_update_status_nodes_task - Background task to add/remove status nodes.
 *
 * Adding or removing device nodes (make_dev/destroy_dev) can be slow or
 * require specific contexts. We use a taskqueue to perform these updates
 * asynchronously to avoid blocking the user thread that triggered the change.
 */
static void
hyrule_update_status_nodes_task(void *context, int pending)
{
	struct hyrule_prop *to_remove = NULL;

	sx_xlock(&hyrule_sx);
	if (hyrule_invincible) {
		if (invincible_node == NULL) {
			add_hyrule_node("characters/link/status/invincible", "1\n");
			struct hyrule_prop *p;
			mtx_lock(&hyrule_mtx);
			LIST_FOREACH(p, &prop_list, next) {
				if (strcmp(p->name, "characters/link/status/invincible") == 0) {
					invincible_node = p;
					break;
				}
			}
			mtx_unlock(&hyrule_mtx);
		}
	} else {
		if (invincible_node != NULL) {
			to_remove = invincible_node;
			invincible_node = NULL;
		}
	}
	sx_xunlock(&hyrule_sx);

	if (to_remove != NULL)
		remove_hyrule_node(to_remove);
}

/**
 * hyrule_load_modules_thread - Background thread for sideloading modules.
 *
 * This function runs as a 'kproc' (kernel process). It attempts to load
 * 'coretemp' and 'amdtemp' modules. We do this in a background thread to:
 * 1. Avoid slowing down the main module load.
 * 2. Avoid complex locking issues during module initialization (MOD_LOAD).
 * 3. Handle cases where the modules might not be available or applicable.
 */
static void
hyrule_load_modules_thread(void *arg)
{
	int fileid;
	/*
	 * kern_kldload is used to dynamically load other kernel modules.
	 * We try both coretemp (Intel) and amdtemp (AMD).
	 * If they are already loaded, we get EEXIST, which is fine.
	 */
	hyrule_coretemp_res = kern_kldload(curthread, "coretemp", &fileid);
	hyrule_amdtemp_res = kern_kldload(curthread, "amdtemp", &fileid);
	kproc_exit(0);
}

void
hyrule_update_status_nodes(void)
{
	taskqueue_enqueue(taskqueue_thread, &status_update_task);
}

/**
 * hyrule_write - Generic write handler for property nodes.
 * @dev: The character device being written to.
 * @uio: The UIO structure describing the data transfer from userland.
 * @ioflag: File status flags.
 *
 * Handles 'echo' or other write operations. It updates the property's value
 * and then calls hyrule_update_prop_state() to trigger any side-effects.
 */
int
hyrule_write(struct cdev *dev, struct uio *uio, int ioflag)
{
	struct hyrule_prop *p = dev->si_drv1;
	int error;
	size_t len;

	if (p == NULL)
		return (ENXIO);

	sx_xlock(&hyrule_sx);

	/* Only allow changes if power is on (except for console commands) */
	if (!hyrule_power && 
	    strcmp(p->name, "console/power") != 0 &&
	    strcmp(p->name, "console/cartridge") != 0 &&
	    strcmp(p->name, "console/reset") != 0) {
		sx_xunlock(&hyrule_sx);
		return (EACCES);
	}

	if (uio->uio_offset >= sizeof(p->value) - 1) {
		sx_xunlock(&hyrule_sx);
		return (EFBIG);
	}
	len = MIN(uio->uio_resid, sizeof(p->value) - 1 - uio->uio_offset);
	error = uiomove(p->value + uio->uio_offset, len, uio);
	p->value[sizeof(p->value) - 1] = '\0';

	if (error != 0) {
		sx_xunlock(&hyrule_sx);
		return (error);
	}

	hyrule_update_prop_state(p, 0);

	sx_xunlock(&hyrule_sx);
	return (0);
}

static d_read_t hyrule_save_read;
static d_write_t hyrule_load_write;
static int hyrule_validate_save(const char *buf, size_t total_len);

static int
hyrule_validate_save(const char *buf, size_t total_len)
{
	const char *p = buf;
	const char *end = buf + total_len;

	while (p < end) {
		/* Skip empty lines and whitespace between blocks */
		while (p < end && (*p == '\n' || *p == '\r' || *p == ' ')) p++;
		if (p >= end) break;

		/* Check "PROP:" */
		if (end - p < 5 || strncmp(p, "PROP:", 5) != 0) return (EINVAL);
		p += 5;
		const char *name_start = p;
		const char *line_end = memchr(p, '\n', end - p);
		if (!line_end) return (EINVAL);
		
		size_t name_len = line_end - name_start;
		if (name_len > 0 && name_start[name_len-1] == '\r') name_len--;
		
		char tmp_name[256];
		if (name_len >= sizeof(tmp_name)) return (EINVAL);
		memcpy(tmp_name, name_start, name_len);
		tmp_name[name_len] = '\0';

		/* Validate property existence */
		int found = 0;
		if (strcmp(tmp_name, "world/map_config") == 0 ||
		    strcmp(tmp_name, "characters/link/status/invincible") == 0) {
			found = 1;
		} else {
			struct hyrule_prop *prop;
			mtx_lock(&hyrule_mtx);
			LIST_FOREACH(prop, &prop_list, next) {
				if (strcmp(prop->name, tmp_name) == 0) {
					found = 1;
					break;
				}
			}
			mtx_unlock(&hyrule_mtx);
		}
		if (!found) return (ENOENT);

		p = line_end + 1;

		/* Check "SIZE:" */
		if (end - p < 5 || strncmp(p, "SIZE:", 5) != 0) return (EINVAL);
		p += 5;
		line_end = memchr(p, '\n', end - p);
		if (!line_end) return (EINVAL);

		size_t val_len = 0;
		for (const char *c = p; c < line_end; c++) {
			if (*c == '\r') continue;
			if (*c < '0' || *c > '9') return (EINVAL);
			val_len = val_len * 10 + (*c - '0');
		}
		if (val_len > 1024) return (EFBIG);

		p = line_end + 1;
		if (end - p < val_len) return (EINVAL);

		p += val_len;
		/* Values should be followed by newline or end of buffer */
		if (p < end && *p != '\n' && *p != '\r') return (EINVAL);
	}
	return (0);
}

/**
 * hyrule_save_read - Handler for /dev/hyrule/game/save.
 *
 * Generates a serialized representation of all mutable game properties.
 * This can be redirected to a file: `cat /dev/hyrule/game/save > my_save.hyrule`
 */
static int
hyrule_save_read(struct cdev *dev, struct uio *uio, int ioflag)
{
	char *buf;
	int len = 0, error;
	struct hyrule_prop *p;

	buf = malloc(16384, M_DEVBUF, M_WAITOK | M_ZERO);

	sx_slock(&hyrule_sx);
	mtx_lock(&hyrule_mtx);
	LIST_FOREACH(p, &prop_list, next) {
		if (strcmp(p->name, "help") == 0 ||
		    strncmp(p->name, "map/", 4) == 0 ||
		    strncmp(p->name, "console/controller/", 19) == 0 ||
		    strcmp(p->name, "console/reset") == 0 ||
		    strcmp(p->name, "game/save") == 0 ||
		    strcmp(p->name, "game/load") == 0)
			continue;

		if (strcmp(p->name, "world/map_config") == 0) {
			char mapbuf[128];
			hyrule_map_get_config(mapbuf, sizeof(mapbuf));
			len += snprintf(buf + len, 16384 - len, "PROP:%s\nSIZE:%zu\n%s\n\n",
			    p->name, strlen(mapbuf), mapbuf);
		} else {
			len += snprintf(buf + len, 16384 - len, "PROP:%s\nSIZE:%zu\n%s\n\n",
			    p->name, strlen(p->value), p->value);
		}
	}
	mtx_unlock(&hyrule_mtx);
	sx_sunlock(&hyrule_sx);

	if (uio->uio_offset >= len) {
		free(buf, M_DEVBUF);
		return (0);
	}
	error = uiomove(buf + uio->uio_offset, len - uio->uio_offset, uio);
	free(buf, M_DEVBUF);
	return (error);
}

/**
 * hyrule_load_write - Handler for /dev/hyrule/game/load.
 *
 * Parses a serialized game state and updates all properties.
 * This can be loaded from a file: `cat my_save.hyrule > /dev/hyrule/game/load`
 */
static int
hyrule_load_write(struct cdev *dev, struct uio *uio, int ioflag)
{
	char *buf, *p_ptr, *line, *name, *val_str;
	size_t len, val_len;
	int error;

	len = uio->uio_resid;
	if (len > 16384) return (EFBIG);

	buf = malloc(len + 1, M_DEVBUF, M_WAITOK | M_ZERO);
	error = uiomove(buf, len, uio);
	if (error) {
		free(buf, M_DEVBUF);
		return (error);
	}
	buf[len] = '\0';

	/* Validation step: reject everything if any part is incorrect */
	error = hyrule_validate_save(buf, len);
	if (error != 0) {
		printf("[HYRULE] Load rejected: Invalid save format or property (error=%d)\n", error);
		free(buf, M_DEVBUF);
		return (error);
	}

	sx_xlock(&hyrule_sx);
	p_ptr = buf;
	while (p_ptr && *p_ptr) {
		/* Skip empty lines */
		while (*p_ptr == '\n' || *p_ptr == '\r' || *p_ptr == ' ') p_ptr++;
		if (*p_ptr == '\0') break;

		line = strsep(&p_ptr, "\n");
		if (line && strncmp(line, "PROP:", 5) == 0) {
			name = line + 5;
			/* Trim possible \r */
			size_t nlen = strlen(name);
			if (nlen > 0 && name[nlen-1] == '\r') name[nlen-1] = '\0';

			line = strsep(&p_ptr, "\n");
			if (line && strncmp(line, "SIZE:", 5) == 0) {
				val_len = strtoul(line + 5, NULL, 10);
				val_str = p_ptr;
				if (val_str && val_len <= strlen(val_str)) {
					p_ptr += val_len;
					char saved = *p_ptr;
					*p_ptr = '\0';

					if (strcmp(name, "world/map_config") == 0) {
						hyrule_map_set_config(val_str, val_len);
					} else if (strcmp(name, "characters/link/status/invincible") == 0) {
						hyrule_invincible = strtol(val_str, NULL, 10);
						hyrule_update_status_nodes();
					} else {
						struct hyrule_prop *prop;
						mtx_lock(&hyrule_mtx);
						LIST_FOREACH(prop, &prop_list, next) {
							if (strcmp(prop->name, name) == 0) {
								strlcpy(prop->value, val_str, sizeof(prop->value));
								hyrule_update_prop_state(prop, 1);
								break;
							}
						}
						mtx_unlock(&hyrule_mtx);
					}
					*p_ptr = saved;
				}
			}
		}
	}
	sx_xunlock(&hyrule_sx);

	free(buf, M_DEVBUF);
	return (0);
}

struct cdevsw hyrule_save_cdevsw = {
	.d_version = D_VERSION,
	.d_open = hyrule_open,
	.d_close = hyrule_close,
	.d_read = hyrule_save_read,
	.d_name = "hyrule_save",
};

struct cdevsw hyrule_load_cdevsw = {
	.d_version = D_VERSION,
	.d_open = hyrule_open,
	.d_close = hyrule_close,
	.d_write = hyrule_load_write,
	.d_name = "hyrule_load",
};

/**
 * add_hyrule_node_custom - Create a new hierarchical property node.
 * @path: The relative path under /dev/hyrule/
 * @initial_val: The starting value for the property.
 * @sw: The device switch (handlers) for this node.
 *
 * This is the core function for creating /dev nodes. It:
 * 1. Allocates a 'hyrule_prop' structure.
 * 2. Initializes 'make_dev_args' to set permissions (0666) and ownership.
 * 3. Calls make_dev_s() to create the actual device entry.
 * 4. Adds the node to the global 'prop_list'.
 */
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
	strlcpy(p->default_value, initial_val, sizeof(p->default_value));

	make_dev_args_init(&args);
	args.mda_flags = MAKEDEV_CHECKNAME | MAKEDEV_WAITOK;
	args.mda_devsw = sw;
	args.mda_uid = UID_ROOT;
	args.mda_gid = GID_WHEEL;
	args.mda_mode = 0666;
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

/**
 * remove_hyrule_node - Destroy a property node and its /dev entry.
 */
void
remove_hyrule_node(struct hyrule_prop *p)
{
	if (p == NULL)
		return;

	mtx_lock(&hyrule_mtx);
	LIST_REMOVE(p, next);
	mtx_unlock(&hyrule_mtx);

	if (p->cdev)
		destroy_dev(p->cdev);
	free(p, M_DEVBUF);
}

/**
 * add_hyrule_node - Shortcut to create a standard property node.
 */
int
add_hyrule_node(const char *path, const char *initial_val)
{
	return add_hyrule_node_custom(path, initial_val, &hyrule_cdevsw);
}

/**
 * hyrule_loader - Module event handler.
 *
 * This is the main entry point for the kernel module. It handles:
 * - MOD_LOAD: Initializing locks, creating all initial /dev/hyrule/ nodes.
 * - MOD_UNLOAD: Draining taskqueues, destroying all nodes, and cleaning up locks.
 * - MOD_SHUTDOWN: System-wide shutdown notification.
 * - MOD_QUIESCE: Checking if it's safe to unload.
 */
static int
hyrule_loader(struct module *mod, int cmd, void *arg)
{
	int error = 0;
	struct hyrule_prop *p;

	switch (cmd) {
	case MOD_LOAD:
		mtx_init(&hyrule_mtx, "hyrule list lock", NULL, MTX_DEF);
		sx_init(&hyrule_sx, "hyrule data lock");
		
		hyrule_map_init();
		hyrule_input_init();

		TASK_INIT(&status_update_task, 0, hyrule_update_status_nodes_task, NULL);

		/* Console */
		error = add_hyrule_node("console/power", "1\n");
		if (error) goto fail;
		error = add_hyrule_node("console/reset", "0\n");
		if (error) goto fail;
		error = add_hyrule_node("console/cartridge", "dusty\n");
		if (error) goto fail;
		error = add_hyrule_node_custom("console/cpu", "", &hyrule_cpu_cdevsw);
		if (error) goto fail;

		/* Help device */
		error = add_hyrule_node("help", 
		    "Welcome to the Hyrule Kernel Module!\n\n"
		    "Map display: cat /dev/hyrule/map/view\n"
		    "World config: /dev/hyrule/world/map_config\n"
		    "Move Link: echo 'up' > /dev/hyrule/characters/link/location/controller\n"
		    "Example: echo 'e' > /dev/hyrule/characters/link/location/controller (to move east)\n\n"
		    "Be careful, it's dangerous to go alone!\n");
		if (error) goto fail;

		/* Map Devices */
		error = add_hyrule_node_custom("map/view", "", &hyrule_map_cdevsw);
		if (error) goto fail;
		error = add_hyrule_node_custom("world/map_config", "", &hyrule_map_config_cdevsw);
		if (error) goto fail;
		error = add_hyrule_node_custom("characters/link/location/controller", "", &hyrule_controller_cdevsw);
		if (error) goto fail;
		
		hyrule_update_controller_nodes();

		error = add_hyrule_node_custom("game/save", "", &hyrule_save_cdevsw);
		if (error) goto fail;
		error = add_hyrule_node_custom("game/load", "", &hyrule_load_cdevsw);
		if (error) goto fail;

		/* Sideload optional temperature modules in background */
		kproc_create(hyrule_load_modules_thread, NULL, NULL, 0, 0, "hyrule_loader");

		/* Characters */
		error = add_hyrule_node("characters/link/stats/health", "3\n");
		if (error) goto fail;
		error = add_hyrule_node("characters/link/stats/stamina", "100\n");
		if (error) goto fail;
		error = add_hyrule_node("characters/link/stats/rupees", "0\n");
		if (error) goto fail;
		error = add_hyrule_node("characters/link/location/x", "0\n");
		if (error) goto fail;
		error = add_hyrule_node("characters/link/location/y", "0\n");
		if (error) goto fail;

		/* Items */
		error = add_hyrule_node("characters/link/items/sword", "None\n");
		if (error) goto fail;
		error = add_hyrule_node("characters/link/stats/sword_level", "0\n");
		if (error) goto fail;
		error = add_hyrule_node("characters/link/items/bombs", "20\n");
		if (error) goto fail;
		error = add_hyrule_node("characters/link/items/bow", "Hero's Bow\n");
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

		/* Status */
		hyrule_update_status_nodes();

		/* Objects */
		error = add_hyrule_node("objects/triforce/parts/collected", "0\n");
		if (error) goto fail;
		error = add_hyrule_node("objects/triforce/parts/courage", "Link\n");
		if (error) goto fail;

		/* World State */
		error = add_hyrule_node("world/dungeon/bosses_defeated", "0\n");
		if (error) goto fail;
		error = add_hyrule_node("world/dungeon/treasures_found", "0\n");
		if (error) goto fail;
		error = add_hyrule_node("characters/link/location/dungeon", "0\n");
		if (error) goto fail;
		error = add_hyrule_node("characters/link/location/room", "0\n");
		if (error) goto fail;

		printf("[HYRULE] Hyrule is now mapped to /dev/hyrule/\n");
		break;

	case MOD_UNLOAD:
		taskqueue_drain(taskqueue_thread, &status_update_task);
		hyrule_map_drain();
		hyrule_input_drain();
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
	taskqueue_drain(taskqueue_thread, &status_update_task);
	hyrule_map_drain();
	hyrule_input_drain();
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
MODULE_VERSION(hyrule, 1);
