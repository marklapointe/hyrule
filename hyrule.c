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

#include <sys/param.h>
#include <sys/module.h>
#include <sys/kernel.h>
#include <sys/systm.h>
#include <sys/conf.h>
#include <sys/uio.h>
#include <sys/malloc.h>
#include <sys/queue.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/sx.h>

/*
 * Hyrule Kernel Module
 * This module creates a hierarchical device tree under /dev/hyrule/
 */

static d_open_t      hyrule_open;
static d_close_t     hyrule_close;
static d_read_t      hyrule_read;
static d_write_t     hyrule_write;

static struct cdevsw hyrule_cdevsw = {
	.d_version = D_VERSION,
	.d_open = hyrule_open,
	.d_close = hyrule_close,
	.d_read = hyrule_read,
	.d_write = hyrule_write,
	.d_name = "hyrule",
};

struct hyrule_prop {
	struct cdev *cdev;
	char name[256];
	char value[512];
	LIST_ENTRY(hyrule_prop) next;
};

static LIST_HEAD(, hyrule_prop) prop_list = LIST_HEAD_INITIALIZER(prop_list);

/* Global locks */
static struct mtx hyrule_mtx;	/* Protects prop_list */
static struct sx hyrule_sx;	/* Protects property values across uiomove */

MTX_SYSINIT(hyrule_mtx, &hyrule_mtx, "hyrule list lock", MTX_DEF);
SX_SYSINIT(hyrule_sx, &hyrule_sx, "hyrule data lock");

static int
hyrule_open(struct cdev *dev, int oflags, int devtype, struct thread *td)
{
	return (0);
}

static int
hyrule_close(struct cdev *dev, int fflag, int devtype, struct thread *td)
{
	return (0);
}

static int
hyrule_read(struct cdev *dev, struct uio *uio, int ioflag)
{
	struct hyrule_prop *p = dev->si_drv1;
	int error;
	size_t len;

	if (p == NULL)
		return (ENXIO);

	/* Use shared lock for reading */
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

static int
hyrule_write(struct cdev *dev, struct uio *uio, int ioflag)
{
	struct hyrule_prop *p = dev->si_drv1;
	int error;
	size_t len;

	if (p == NULL)
		return (ENXIO);

	/* Use exclusive lock for writing */
	sx_xlock(&hyrule_sx);

	if (uio->uio_offset >= sizeof(p->value) - 1) {
		sx_xunlock(&hyrule_sx);
		return (EFBIG);
	}

	/* Limit write size to remaining buffer space */
	len = MIN(uio->uio_resid, sizeof(p->value) - 1 - uio->uio_offset);
	
	error = uiomove(p->value + uio->uio_offset, len, uio);
	
	/* Ensure null termination */
	p->value[sizeof(p->value) - 1] = '\0';

	sx_xunlock(&hyrule_sx);
	return (error);
}

static int
add_hyrule_node(const char *path, const char *initial_val)
{
	struct hyrule_prop *p;
	struct make_dev_args args;
	int error;

	/* Basic input validation */
	if (path == NULL || initial_val == NULL)
		return (EINVAL);

	p = malloc(sizeof(*p), M_DEVBUF, M_WAITOK | M_ZERO);
	if (p == NULL)
		return (ENOMEM);

	strlcpy(p->name, path, sizeof(p->name));
	strlcpy(p->value, initial_val, sizeof(p->value));

	/* 
	 * Use make_dev_s with MAKEDEV_CHECKNAME to avoid kernel panic if the name 
	 * is invalid or already exists. This fixes the issue where an invalid 
	 * si_name would trigger a panic.
	 */
	make_dev_args_init(&args);
	args.mda_flags = MAKEDEV_CHECKNAME | MAKEDEV_WAITOK;
	args.mda_devsw = &hyrule_cdevsw;
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

static int
hyrule_loader(struct module *mod, int cmd, void *arg)
{
	int error = 0;
	struct hyrule_prop *p;

	switch (cmd) {
	case MOD_LOAD:
		printf("[HYRULE] It's dangerous to go alone! Take this module.\n");
		
		/* 
		 * Create the generic structure: 
		 * /dev/hyrule/{type}/{name}/{category}/{property}
		 */
		
		/* Help device */
		error = add_hyrule_node("help", 
		    "Welcome to the Hyrule Kernel Module!\n\n"
		    "This module maps the Land of Hyrule into your /dev filesystem.\n"
		    "You can interact with characters and objects by reading and writing to their properties.\n\n"
		    "Structure: /dev/hyrule/{type}/{name}/{category}/{property}\n\n"
		    "Examples:\n"
		    "- Read Link's sword: cat /dev/hyrule/characters/link/weapons/sword\n"
		    "- Slay Ganon: echo 'SLAIN' > /dev/hyrule/characters/ganon/status/condition\n\n"
		    "Be careful, it's dangerous to go alone!\n");
		if (error) goto fail;

		/* Link's Stats and Gear */
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

		/* Zelda's Stats and Gear */
		error = add_hyrule_node("characters/zelda/stats/health", "100\n");
		if (error) goto fail;
		error = add_hyrule_node("characters/zelda/stats/magic", "100\n");
		if (error) goto fail;
		error = add_hyrule_node("characters/zelda/magic/light_arrow", "Holy Arrows\n");
		if (error) goto fail;

		/* Objects */
		error = add_hyrule_node("objects/triforce/parts/courage", "Link\n");
		if (error) goto fail;

		/* Ganon's Stats */
		error = add_hyrule_node("characters/ganon/stats/health", "200\n");
		if (error) goto fail;
		error = add_hyrule_node("characters/ganon/stats/power", "100\n");
		if (error) goto fail;
		error = add_hyrule_node("characters/ganon/status/condition", "ALIVE\n");
		if (error) goto fail;

		printf("[HYRULE] Hyrule is now mapped to /dev/hyrule/\n");
		break;

	case MOD_UNLOAD:
		/* 
		 * Safe cleanup: remove from list first, then destroy dev.
		 * destroy_dev() can sleep, so we must not hold the mutex.
		 */
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
		printf("[HYRULE] Link has saved the game. Leaving the kernel...\n");
		break;

	default:
		error = EOPNOTSUPP;
		break;
	}
	return (error);

fail:
	/* Cleanup on partial load using the same safe logic */
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
	return (error);
}

static moduledata_t hyrule_mod = {
	"hyrule",
	hyrule_loader,
	NULL
};

DECLARE_MODULE(hyrule, hyrule_mod, SI_SUB_DRIVERS, SI_ORDER_MIDDLE);
