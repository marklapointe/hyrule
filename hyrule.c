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
#include <sys/kthread.h>
#include <sys/proc.h>

/*
 * Hyrule Kernel Module Core
 */

struct prop_head prop_list = LIST_HEAD_INITIALIZER(prop_list);

/* Global locks */
struct mtx hyrule_mtx;	/* Protects prop_list */
struct sx hyrule_sx;	/* Protects property values across uiomove */

static struct cdevsw hyrule_cdevsw;

int hyrule_power = 1;
int hyrule_cartridge = 0; /* Starts dusty */
int hyrule_invincible = 0;

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

static void hyrule_update_prop_state(struct hyrule_prop *p, int loading);

void
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

void
hyrule_update_status_nodes(void)
{
	taskqueue_enqueue(taskqueue_thread, &status_update_task);
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
