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
 */

/* 
 * Variables
 */

/* Global list of all properties (nodes) created by the module. */
struct propHead propList = LIST_HEAD_INITIALIZER(propList);

/* Global locks */
struct mtx hyruleMtx;	/* Protects propList structural integrity */
struct sx hyruleSx;	/* Protects property 'value' contents across sleepable I/O */

/* Status tracking for optional temperature modules. */
static int hyruleCoretempRes = -1; /* -1: not attempted, 0/EEXIST: success, errno: error */
static int hyruleAmdtempRes = -1;

/* Global game state variables */
int hyrulePower = 1;      /* System power state (0=off, 1=on) */
int hyruleCartridge = 0;  /* Cartridge cleanliness (0=dusty, 1=clean) */
int hyruleInvincible = 0; /* Cheat mode active? */

/* Internal state for node management */
static struct task statusUpdateTask;
static struct hyruleProp *invincibleNode = NULL;

/* 
 * Device Switches (cdevsw)
 */

/* Specialized cdevsw for the CPU stats node. */
static d_read_t hyruleCpuRead;
static struct cdevsw hyruleCpuCdevsw = {
	.d_version = D_VERSION,
	.d_open = hyruleOpen,
	.d_close = hyruleClose,
	.d_read = hyruleCpuRead,
	.d_name = "hyrule_cpu",
};

/* The main cdevsw for standard property nodes. */
static struct cdevsw hyruleCdevsw = {
	.d_version = D_VERSION,
	.d_open = hyruleOpen,
	.d_close = hyruleClose,
	.d_read = hyruleRead,
	.d_write = hyruleWrite,
	.d_ioctl = hyruleIoctl,
	.d_poll = hyrulePoll,
	.d_mmap = hyruleMmap,
	.d_strategy = hyruleStrategy,
	.d_kqfilter = hyruleKqfilter,
	.d_fdopen = hyruleFdopen,
	.d_mmap_single = hyruleMmapSingle,
	.d_purge = hyrulePurge,
	.d_name = "hyrule",
};

/* Prototypes for save/load */
static d_read_t hyruleSaveRead;
static d_write_t hyruleLoadWrite;

struct cdevsw hyruleSaveCdevsw = {
	.d_version = D_VERSION,
	.d_open = hyruleOpen,
	.d_close = hyruleClose,
	.d_read = hyruleSaveRead,
	.d_name = "hyrule_save",
};

struct cdevsw hyruleLoadCdevsw = {
	.d_version = D_VERSION,
	.d_open = hyruleOpen,
	.d_close = hyruleClose,
	.d_write = hyruleLoadWrite,
	.d_name = "hyrule_load",
};

/*
 * Methods/Functions (Prototypes)
 */
static void hyruleUpdateStatusNodesTask(void *context, int pending);
static void hyruleUpdatePropState(struct hyruleProp *p, int loading);
static void hyruleLoadModulesThread(void *arg);
static int hyruleValidateSave(const char *buf, size_t totalLen);
static int hyruleLoader(struct module *mod, int cmd, void *arg);

/*
 * Methods/Functions (Definitions)
 */

/**
 * hyruleGetPropInt - Safely retrieve an integer value from a property node.
 */
int
hyruleGetPropInt(const char *name, int defaultVal)
{
	struct hyruleProp *p;
	int val = defaultVal;

	mtx_lock(&hyruleMtx);
	LIST_FOREACH(p, &propList, next) {
		if (strcmp(p->name, name) == 0) {
			val = strtol(p->value, NULL, 10);
			break;
		}
	}
	mtx_unlock(&hyruleMtx);
	return (val);
}

/**
 * hyruleSetPropInt - Safely update a property node with an integer value.
 */
void
hyruleSetPropInt(const char *name, int val)
{
	struct hyruleProp *p;

	mtx_lock(&hyruleMtx);
	LIST_FOREACH(p, &propList, next) {
		if (strcmp(p->name, name) == 0) {
			snprintf(p->value, sizeof(p->value), "%d\n", val);
			break;
		}
	}
	mtx_unlock(&hyruleMtx);
}

/**
 * hyruleIsActive - Check if the game is in a playable state.
 */
int
hyruleIsActive(void)
{
	struct hyruleProp *p;
	long hp = 1;

	if (hyrulePower == 0)
		return (0);

	if (hyruleInvincible)
		return (1);

	mtx_lock(&hyruleMtx);
	LIST_FOREACH(p, &propList, next) {
		if (strcmp(p->name, "characters/link/stats/health") == 0) {
			hp = strtol(p->value, NULL, 10);
		}
	}
	mtx_unlock(&hyruleMtx);

	return (hp > 0);
}

/**
 * hyruleReset - Restore all properties to their default values.
 */
void
hyruleReset(void)
{
	struct hyruleProp *p;

	mtx_lock(&hyruleMtx);
	LIST_FOREACH(p, &propList, next) {
		strlcpy(p->value, p->defaultValue, sizeof(p->value));
	}
	mtx_unlock(&hyruleMtx);

	hyruleInvincible = 0;
	hyruleUpdateStatusNodes();
	hyruleMapInit();
	
	printf("[HYRULE] System Reset. Welcome back!\n");
}

/**
 * hyruleOpen - Called when a user opens a /dev/hyrule/ device.
 */
int
hyruleOpen(struct cdev *dev, int oflags, int devtype, struct thread *td)
{
	return (0);
}

/**
 * hyruleClose - Called when the last file descriptor for the device is closed.
 */
int
hyruleClose(struct cdev *dev, int fflag, int devtype, struct thread *td)
{
	return (0);
}

int
hyruleIoctl(struct cdev *dev, u_long cmd, caddr_t data, int fflag, struct thread *td)
{
	return (ENOTTY);
}

int
hyrulePoll(struct cdev *dev, int events, struct thread *td)
{
	return (events & (POLLIN | POLLRDNORM | POLLOUT | POLLWRNORM));
}

int
hyruleMmap(struct cdev *dev, vm_ooffset_t offset, vm_paddr_t *paddr, int nprot, vm_memattr_t *memattr)
{
	return (EINVAL);
}

int
hyruleKqfilter(struct cdev *dev, struct knote *kn)
{
	return (EINVAL);
}

void
hyruleStrategy(struct bio *bp)
{
	biofinish(bp, NULL, ENODEV);
}

int
hyruleFdopen(struct cdev *dev, int oflags, struct thread *td, struct file *fp)
{
	return (0);
}

int
hyruleMmapSingle(struct cdev *cdev, vm_ooffset_t *offset, vm_size_t size, struct vm_object **object, int nprot)
{
	return (ENODEV);
}

void
hyrulePurge(struct cdev *dev)
{
}

/**
 * hyruleRead - Generic read handler for property nodes.
 */
int
hyruleRead(struct cdev *dev, struct uio *uio, int ioflag)
{
	struct hyruleProp *p = dev->si_drv1;
	int error;
	size_t len;

	if (p == NULL)
		return (ENXIO);

	sx_slock(&hyruleSx);

	/* Check if system is active */
	if (!hyruleIsActive() && 
	    strcmp(p->name, "console/power") != 0 && 
	    strcmp(p->name, "console/reset") != 0 &&
	    strcmp(p->name, "console/cartridge") != 0 &&
	    strcmp(p->name, "help") != 0) {
		const char *msg;
		if (hyrulePower == 0)
			msg = "POWER OFF\n";
		else
			msg = "GAME OVER\n";
		
		len = strlen(msg);
		if (uio->uio_offset >= len) {
			sx_sunlock(&hyruleSx);
			return (0);
		}
		error = uiomove(__DECONST(char *, msg) + uio->uio_offset, len - uio->uio_offset, uio);
		sx_sunlock(&hyruleSx);
		return (error);
	}

	len = strnlen(p->value, sizeof(p->value));
	if (uio->uio_offset >= len) {
		sx_sunlock(&hyruleSx);
		return (0);
	}
	error = uiomove(p->value + uio->uio_offset, len - uio->uio_offset, uio);
	sx_sunlock(&hyruleSx);
	return (error);
}

/**
 * hyruleCpuRead - Specialized read handler for /dev/hyrule/console/cpu.
 */
static int
hyruleCpuRead(struct cdev *dev, struct uio *uio, int ioflag)
{
	struct sbuf *sb;
	long *times;
	int error;
	size_t len;
	size_t timesSz;

	timesSz = mp_ncpus * CPUSTATES * sizeof(long);
	times = malloc(timesSz, M_DEVBUF, M_WAITOK | M_ZERO);

	if (kernel_sysctlbyname(curthread, "kern.cp_times", times, &timesSz, NULL, 0, NULL, 0) != 0) {
		memset(times, 0, timesSz);
	}

	sb = sbuf_new_auto();
	if (sb == NULL) {
		free(times, M_DEVBUF);
		return (ENOMEM);
	}

	sbuf_printf(sb, "{\n");
	sbuf_printf(sb, "  \"ncpus\": %d,\n", mp_ncpus);
	
	sbuf_printf(sb, "  \"drivers\": {\n");
	sbuf_printf(sb, "    \"coretemp\": \"%s\",\n", 
	    (hyruleCoretempRes == 0 || hyruleCoretempRes == EEXIST) ? "loaded" : 
	    (hyruleCoretempRes == -1) ? "pending" : "not found");
	sbuf_printf(sb, "    \"amdtemp\": \"%s\"\n",
	    (hyruleAmdtempRes == 0 || hyruleAmdtempRes == EEXIST) ? "loaded" :
	    (hyruleAmdtempRes == -1) ? "pending" : "not found");
	sbuf_printf(sb, "  },\n");

	sbuf_printf(sb, "  \"cpus\": [\n");

	for (int i = 0; i < mp_ncpus; i++) {
		int temp = 0;
		size_t tempSz = sizeof(temp);
		char name[64];

		snprintf(name, sizeof(name), "dev.cpu.%d.temperature", i);
		error = kernel_sysctlbyname(curthread, name, &temp, &tempSz, NULL, 0, NULL, 0);

		sbuf_printf(sb, "    {\n      \"id\": %d,\n", i);
		if (error == 0) {
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
 * hyruleUpdatePropState - Process side-effects of property changes.
 */
static void
hyruleUpdatePropState(struct hyruleProp *p, int loading)
{
	if (strcmp(p->name, "console/power") == 0) {
		int newPower = strtol(p->value, NULL, 10);
		if (newPower != hyrulePower) {
			if (newPower == 1 && !loading) {
				if (hyruleCartridge == 0 && (arc4random() % 100) < 80) {
					printf("[HYRULE] Power-on failed. Red light blinking. Cartridge dusty?\n");
					strlcpy(p->value, "0\n", sizeof(p->value));
					return;
				}
				hyruleReset();
			}
			hyrulePower = newPower;
			printf("[HYRULE] Power set to %d\n", hyrulePower);
			if (hyrulePower == 0 && !loading) {
				if ((arc4random() % 100) < 50) {
					hyruleCartridge = 0;
					struct hyruleProp *cp;
					mtx_lock(&hyruleMtx);
					LIST_FOREACH(cp, &propList, next) {
						if (strcmp(cp->name, "console/cartridge") == 0) {
							strlcpy(cp->value, "dusty\n", sizeof(cp->value));
							break;
						}
					}
					mtx_unlock(&hyruleMtx);
					printf("[HYRULE] Cartridge is now dusty after power cycle.\n");
				}
			}
		}
	} else if (strcmp(p->name, "console/reset") == 0) {
		if (strtol(p->value, NULL, 10) == 1) {
			hyruleReset();
			strlcpy(p->value, "0\n", sizeof(p->value));
		}
	} else if (strcmp(p->name, "console/cartridge") == 0) {
		if (strncmp(p->value, "blow", 4) == 0 || strncmp(p->value, "clean", 5) == 0) {
			hyruleCartridge = 1;
			if (strncmp(p->value, "blow", 4) == 0)
				strlcpy(p->value, "clean\n", sizeof(p->value));
			printf("[HYRULE] Cartridge is now clean and ready to play!\n");
		} else {
			hyruleCartridge = 0;
			strlcpy(p->value, "dusty\n", sizeof(p->value));
		}
	} else if (strcmp(p->name, "characters/link/stats/health") == 0) {
		long hp = strtol(p->value, NULL, 10);
		if (hp <= 0) {
			printf("[HYRULE] Link has no life left! GAME OVER.\n");
		}
	} else if (strcmp(p->name, "characters/link/status/invincible") == 0) {
		int newInvincible = strtol(p->value, NULL, 10);
		if (newInvincible != hyruleInvincible) {
			hyruleInvincible = newInvincible;
			hyruleUpdateStatusNodes();
		}
	}
}

/**
 * hyruleUpdateStatusNodesTask - Background task to add/remove status nodes.
 */
static void
hyruleUpdateStatusNodesTask(void *context, int pending)
{
	struct hyruleProp *toRemove = NULL;

	sx_xlock(&hyruleSx);
	if (hyruleInvincible) {
		if (invincibleNode == NULL) {
			addHyruleNode("characters/link/status/invincible", "1\n");
			struct hyruleProp *p;
			mtx_lock(&hyruleMtx);
			LIST_FOREACH(p, &propList, next) {
				if (strcmp(p->name, "characters/link/status/invincible") == 0) {
					invincibleNode = p;
					break;
				}
			}
			mtx_unlock(&hyruleMtx);
		}
	} else {
		if (invincibleNode != NULL) {
			toRemove = invincibleNode;
			invincibleNode = NULL;
		}
	}
	sx_xunlock(&hyruleSx);

	if (toRemove != NULL)
		removeHyruleNode(toRemove);
}

/**
 * hyruleLoadModulesThread - Background thread for sideloading modules.
 */
static void
hyruleLoadModulesThread(void *arg)
{
	int fileid;
	hyruleCoretempRes = kern_kldload(curthread, "coretemp", &fileid);
	hyruleAmdtempRes = kern_kldload(curthread, "amdtemp", &fileid);
	kproc_exit(0);
}

void
hyruleUpdateStatusNodes(void)
{
	taskqueue_enqueue(taskqueue_thread, &statusUpdateTask);
}

/**
 * hyruleWrite - Generic write handler for property nodes.
 */
int
hyruleWrite(struct cdev *dev, struct uio *uio, int ioflag)
{
	struct hyruleProp *p = dev->si_drv1;
	int error;
	size_t len;

	if (p == NULL)
		return (ENXIO);

	sx_xlock(&hyruleSx);

	if (!hyrulePower && 
	    strcmp(p->name, "console/power") != 0 &&
	    strcmp(p->name, "console/cartridge") != 0 &&
	    strcmp(p->name, "console/reset") != 0) {
		sx_xunlock(&hyruleSx);
		return (EACCES);
	}

	if (uio->uio_offset >= sizeof(p->value) - 1) {
		sx_xunlock(&hyruleSx);
		return (EFBIG);
	}
	len = MIN(uio->uio_resid, sizeof(p->value) - 1 - uio->uio_offset);
	error = uiomove(p->value + uio->uio_offset, len, uio);
	p->value[sizeof(p->value) - 1] = '\0';

	if (error != 0) {
		sx_xunlock(&hyruleSx);
		return (error);
	}

	hyruleUpdatePropState(p, 0);

	sx_xunlock(&hyruleSx);
	return (0);
}

/**
 * hyruleValidateSave - Parser validation.
 */
static int
hyruleValidateSave(const char *buf, size_t totalLen)
{
	const char *p = buf;
	const char *end = buf + totalLen;

	while (p < end) {
		while (p < end && (*p == '\n' || *p == '\r' || *p == ' ')) p++;
		if (p >= end) break;

		if (end - p < 5 || strncmp(p, "PROP:", 5) != 0) return (EINVAL);
		p += 5;
		const char *nameStart = p;
		const char *lineEnd = memchr(p, '\n', end - p);
		if (!lineEnd) return (EINVAL);
		
		size_t nameLen = lineEnd - nameStart;
		if (nameLen > 0 && nameStart[nameLen-1] == '\r') nameLen--;
		
		char tmpName[256];
		if (nameLen >= sizeof(tmpName)) return (EINVAL);
		memcpy(tmpName, nameStart, nameLen);
		tmpName[nameLen] = '\0';

		int found = 0;
		if (strcmp(tmpName, "world/map_config") == 0 ||
		    strcmp(tmpName, "characters/link/status/invincible") == 0) {
			found = 1;
		} else {
			struct hyruleProp *prop;
			mtx_lock(&hyruleMtx);
			LIST_FOREACH(prop, &propList, next) {
				if (strcmp(prop->name, tmpName) == 0) {
					found = 1;
					break;
				}
			}
			mtx_unlock(&hyruleMtx);
		}
		if (!found) return (ENOENT);

		p = lineEnd + 1;

		if (end - p < 5 || strncmp(p, "SIZE:", 5) != 0) return (EINVAL);
		p += 5;
		lineEnd = memchr(p, '\n', end - p);
		if (!lineEnd) return (EINVAL);

		size_t valLen = 0;
		for (const char *c = p; c < lineEnd; c++) {
			if (*c == '\r') continue;
			if (*c < '0' || *c > '9') return (EINVAL);
			valLen = valLen * 10 + (*c - '0');
		}
		if (valLen > 1024) return (EFBIG);

		p = lineEnd + 1;
		if (end - p < valLen) return (EINVAL);

		p += valLen;
		if (p < end && *p != '\n' && *p != '\r') return (EINVAL);
	}
	return (0);
}

/**
 * hyruleSaveRead - Handler for /dev/hyrule/game/save.
 */
static int
hyruleSaveRead(struct cdev *dev, struct uio *uio, int ioflag)
{
	char *buf;
	int len = 0, error;
	struct hyruleProp *p;

	buf = malloc(16384, M_DEVBUF, M_WAITOK | M_ZERO);

	sx_slock(&hyruleSx);
	mtx_lock(&hyruleMtx);
	LIST_FOREACH(p, &propList, next) {
		if (strcmp(p->name, "help") == 0 ||
		    strncmp(p->name, "map/", 4) == 0 ||
		    strncmp(p->name, "console/controller/", 19) == 0 ||
		    strcmp(p->name, "console/reset") == 0 ||
		    strcmp(p->name, "game/save") == 0 ||
		    strcmp(p->name, "game/load") == 0)
			continue;

		if (strcmp(p->name, "world/map_config") == 0) {
			char mapbuf[128];
			hyruleMapGetConfig(mapbuf, sizeof(mapbuf));
			len += snprintf(buf + len, 16384 - len, "PROP:%s\nSIZE:%zu\n%s\n\n",
			    p->name, strlen(mapbuf), mapbuf);
		} else {
			len += snprintf(buf + len, 16384 - len, "PROP:%s\nSIZE:%zu\n%s\n\n",
			    p->name, strlen(p->value), p->value);
		}
	}
	mtx_unlock(&hyruleMtx);
	sx_sunlock(&hyruleSx);

	if (uio->uio_offset >= len) {
		free(buf, M_DEVBUF);
		return (0);
	}
	error = uiomove(buf + uio->uio_offset, len - uio->uio_offset, uio);
	free(buf, M_DEVBUF);
	return (error);
}

/**
 * hyruleLoadWrite - Handler for /dev/hyrule/game/load.
 */
static int
hyruleLoadWrite(struct cdev *dev, struct uio *uio, int ioflag)
{
	char *buf, *pPtr, *line, *name, *valStr;
	size_t len, valLen;
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

	error = hyruleValidateSave(buf, len);
	if (error != 0) {
		printf("[HYRULE] Load rejected: Invalid save format or property (error=%d)\n", error);
		free(buf, M_DEVBUF);
		return (error);
	}

	sx_xlock(&hyruleSx);
	pPtr = buf;
	while (pPtr && *pPtr) {
		while (*pPtr == '\n' || *pPtr == '\r' || *pPtr == ' ') pPtr++;
		if (*pPtr == '\0') break;

		line = strsep(&pPtr, "\n");
		if (line && strncmp(line, "PROP:", 5) == 0) {
			name = line + 5;
			size_t nlen = strlen(name);
			if (nlen > 0 && name[nlen-1] == '\r') name[nlen-1] = '\0';

			line = strsep(&pPtr, "\n");
			if (line && strncmp(line, "SIZE:", 5) == 0) {
				valLen = strtoul(line + 5, NULL, 10);
				valStr = pPtr;
				if (valStr && valLen <= strlen(valStr)) {
					pPtr += valLen;
					char saved = *pPtr;
					*pPtr = '\0';

					if (strcmp(name, "world/map_config") == 0) {
						hyruleMapSetConfig(valStr, valLen);
					} else if (strcmp(name, "characters/link/status/invincible") == 0) {
						hyruleInvincible = strtol(valStr, NULL, 10);
						hyruleUpdateStatusNodes();
					} else {
						struct hyruleProp *prop;
						mtx_lock(&hyruleMtx);
						LIST_FOREACH(prop, &propList, next) {
							if (strcmp(prop->name, name) == 0) {
								strlcpy(prop->value, valStr, sizeof(prop->value));
								hyruleUpdatePropState(prop, 1);
								break;
							}
						}
						mtx_unlock(&hyruleMtx);
					}
					*pPtr = saved;
				}
			}
		}
	}
	sx_xunlock(&hyruleSx);

	free(buf, M_DEVBUF);
	return (0);
}

/**
 * addHyruleNodeCustom - Create a new hierarchical property node.
 */
int
addHyruleNodeCustom(const char *path, const char *initialVal, struct cdevsw *sw)
{
	struct hyruleProp *p;
	struct make_dev_args args;
	int error;

	if (path == NULL || initialVal == NULL)
		return (EINVAL);

	p = malloc(sizeof(*p), M_DEVBUF, M_WAITOK | M_ZERO);
	if (p == NULL)
		return (ENOMEM);

	strlcpy(p->name, path, sizeof(p->name));
	strlcpy(p->value, initialVal, sizeof(p->value));
	strlcpy(p->defaultValue, initialVal, sizeof(p->defaultValue));

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

	mtx_lock(&hyruleMtx);
	LIST_INSERT_HEAD(&propList, p, next);
	mtx_unlock(&hyruleMtx);
	return (0);
}

/**
 * removeHyruleNode - Destroy a property node and its /dev entry.
 */
void
removeHyruleNode(struct hyruleProp *p)
{
	if (p == NULL)
		return;

	mtx_lock(&hyruleMtx);
	LIST_REMOVE(p, next);
	mtx_unlock(&hyruleMtx);

	if (p->cdev)
		destroy_dev(p->cdev);
	free(p, M_DEVBUF);
}

/**
 * addHyruleNode - Shortcut to create a standard property node.
 */
int
addHyruleNode(const char *path, const char *initialVal)
{
	return addHyruleNodeCustom(path, initialVal, &hyruleCdevsw);
}

/**
 * hyruleLoader - Module event handler.
 */
static int
hyruleLoader(struct module *mod, int cmd, void *arg)
{
	int error = 0;
	struct hyruleProp *p;

	switch (cmd) {
	case MOD_LOAD:
		mtx_init(&hyruleMtx, "hyrule list lock", NULL, MTX_DEF);
		sx_init(&hyruleSx, "hyrule data lock");
		
		hyruleMapInit();
		hyruleInputInit();

		TASK_INIT(&statusUpdateTask, 0, hyruleUpdateStatusNodesTask, NULL);

		error = addHyruleNode("console/power", "1\n");
		if (error) goto fail;
		error = addHyruleNode("console/reset", "0\n");
		if (error) goto fail;
		error = addHyruleNode("console/cartridge", "dusty\n");
		if (error) goto fail;
		error = addHyruleNodeCustom("console/cpu", "", &hyruleCpuCdevsw);
		if (error) goto fail;

		error = addHyruleNode("help", 
		    "Welcome to the Hyrule Kernel Module!\n\n"
		    "Map display: cat /dev/hyrule/map/view\n"
		    "World config: /dev/hyrule/world/map_config\n"
		    "Move Link: echo 'up' > /dev/hyrule/characters/link/location/controller\n\n"
		    "Be careful, it's dangerous to go alone!\n");
		if (error) goto fail;

		error = addHyruleNodeCustom("map/view", "", &hyruleMapCdevsw);
		if (error) goto fail;
		error = addHyruleNodeCustom("world/map_config", "", &hyruleMapConfigCdevsw);
		if (error) goto fail;
		error = addHyruleNodeCustom("characters/link/location/controller", "", &hyruleControllerCdevsw);
		if (error) goto fail;
		
		hyruleUpdateControllerNodes();

		error = addHyruleNodeCustom("game/save", "", &hyruleSaveCdevsw);
		if (error) goto fail;
		error = addHyruleNodeCustom("game/load", "", &hyruleLoadCdevsw);
		if (error) goto fail;

		kproc_create(hyruleLoadModulesThread, NULL, NULL, 0, 0, "hyrule_loader");

		error = addHyruleNode("characters/link/stats/health", "3\n");
		if (error) goto fail;
		error = addHyruleNode("characters/link/stats/stamina", "100\n");
		if (error) goto fail;
		error = addHyruleNode("characters/link/stats/rupees", "0\n");
		if (error) goto fail;
		error = addHyruleNode("characters/link/location/x", "0\n");
		if (error) goto fail;
		error = addHyruleNode("characters/link/location/y", "0\n");
		if (error) goto fail;

		error = addHyruleNode("characters/link/items/sword", "None\n");
		if (error) goto fail;
		error = addHyruleNode("characters/link/stats/sword_level", "0\n");
		if (error) goto fail;

		error = addHyruleNode("characters/zelda/stats/health", "100\n");
		if (error) goto fail;
		error = addHyruleNode("characters/ganon/stats/health", "200\n");
		if (error) goto fail;

		hyruleUpdateStatusNodes();

		printf("[HYRULE] Hyrule is now mapped to /dev/hyrule/\n");
		break;

	case MOD_UNLOAD:
		taskqueue_drain(taskqueue_thread, &statusUpdateTask);
		hyruleMapDrain();
		hyruleInputDrain();
		while (1) {
			mtx_lock(&hyruleMtx);
			p = LIST_FIRST(&propList);
			if (p == NULL) {
				mtx_unlock(&hyruleMtx);
				break;
			}
			LIST_REMOVE(p, next);
			mtx_unlock(&hyruleMtx);
			destroy_dev(p->cdev);
			free(p, M_DEVBUF);
		}
		mtx_destroy(&hyruleMtx);
		sx_destroy(&hyruleSx);
		printf("[HYRULE] Link has saved the game. Leaving the kernel...\n");
		break;

	case MOD_SHUTDOWN:
		printf("[HYRULE] Hyrule is fading as the system shuts down...\n");
		break;

	case MOD_QUIESCE:
		printf("[HYRULE] Checking if Link is ready to leave...\n");
		error = 0;
		break;

	default:
		error = EOPNOTSUPP;
		break;
	}
	return (error);

fail:
	taskqueue_drain(taskqueue_thread, &statusUpdateTask);
	hyruleMapDrain();
	hyruleInputDrain();
	while (1) {
		mtx_lock(&hyruleMtx);
		p = LIST_FIRST(&propList);
		if (p == NULL) {
			mtx_unlock(&hyruleMtx);
			break;
		}
		LIST_REMOVE(p, next);
		mtx_unlock(&hyruleMtx);
		if (p->cdev)
			destroy_dev(p->cdev);
		free(p, M_DEVBUF);
	}
	mtx_destroy(&hyruleMtx);
	sx_destroy(&hyruleSx);
	return (error);
}

static moduledata_t hyruleMod = {
	"hyrule",
	hyruleLoader,
	NULL
};

DECLARE_MODULE(hyrule, hyruleMod, SI_SUB_DRIVERS, SI_ORDER_MIDDLE);
MODULE_VERSION(hyrule, 1);
