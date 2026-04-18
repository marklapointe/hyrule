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
struct propertyHead propertyList = LIST_HEAD_INITIALIZER(propertyList);

/* Global locks */
struct mtx hyruleMutex;	/* Protects propertyList structural integrity */
struct sx hyruleSharedExclusion;	/* Protects property 'value' contents across sleepable I/O */

/* Status tracking for optional temperature modules. */
static int hyruleCoretempResult = -1; /* -1: not attempted, 0/EEXIST: success, errno: error */
static int hyruleAmdtempResult = -1;

/* Global game state variables */
int hyrulePower = 1;      /* System power state (0=off, 1=on) */
int hyruleCartridge = 0;  /* Cartridge cleanliness (0=dusty, 1=clean) */
int hyruleInvincible = 0; /* Cheat mode active? */

/* Internal state for node management */
static struct task statusUpdateTaskQueueItem;
static struct hyruleProperty *invinciblePropertyNode = NULL;

/* 
 * Device Switches (characterDeviceSwitch)
 */

/* Specialized characterDeviceSwitch for the CPU stats node. */
static d_read_t hyruleCpuRead;
static struct cdevsw hyruleCpuCharacterDeviceSwitch = {
	.d_version = D_VERSION,
	.d_open = hyruleOpen,
	.d_close = hyruleClose,
	.d_read = hyruleCpuRead,
	.d_name = "hyrule_cpu",
};

/* The main characterDeviceSwitch for standard property nodes. */
static struct cdevsw hyruleCharacterDeviceSwitch = {
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

struct cdevsw hyruleSaveCharacterDeviceSwitch = {
	.d_version = D_VERSION,
	.d_open = hyruleOpen,
	.d_close = hyruleClose,
	.d_read = hyruleSaveRead,
	.d_name = "hyrule_save",
};

struct cdevsw hyruleLoadCharacterDeviceSwitch = {
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
static void hyruleUpdatePropState(struct hyruleProperty *property, int loading);
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
	struct hyruleProperty *property;
	int val = defaultVal;

	/* 
	 * We acquire the mutex to safely traverse the global property list.
	 * This prevents other threads from modifying the list structure (e.g., adding 
	 * or removing nodes) while we are searching through it.
	 */
	mtx_lock(&hyruleMutex);
	LIST_FOREACH(property, &propertyList, next) {
		/* Compare the name of the current property with the target name. */
		if (strcmp(property->name, name) == 0) {
			/* If found, convert the string value to an integer. */
			val = strtol(property->value, NULL, 10);
			break;
		}
	}
	/* Release the lock after we've finished our search or found the item. */
	mtx_unlock(&hyruleMutex);
	return (val);
}

/**
 * hyruleSetPropInt - Safely update a property node with an integer value.
 */
void
hyruleSetPropInt(const char *name, int val)
{
	struct hyruleProperty *property;

	/* 
	 * Lock the property list to ensure we can safely iterate through it.
	 */
	mtx_lock(&hyruleMutex);
	LIST_FOREACH(property, &propertyList, next) {
		/* If the property name matches, update its value. */
		if (strcmp(property->name, name) == 0) {
			/* Values are stored as strings for easy retrieval by userspace. */
			snprintf(property->value, sizeof(property->value), "%d\n", val);
			break;
		}
	}
	/* Release the structural lock. */
	mtx_unlock(&hyruleMutex);
}

/**
 * hyruleIsActive - Check if the game is in a playable state.
 */
int
hyruleIsActive(void)
{
	struct hyruleProperty *property;
	long hp = 1;

	/* If the main power is off, the system is not active. */
	if (hyrulePower == 0)
		return (0);

	/* If cheat mode (invincibility) is active, the game is always considered playable. */
	if (hyruleInvincible)
		return (1);

	/* 
	 * Otherwise, we check the main character's health. If health is 0 or less, 
	 * it's a 'game over' state.
	 */
	mtx_lock(&hyruleMutex);
	LIST_FOREACH(property, &propertyList, next) {
		if (strcmp(property->name, "characters/link/stats/health") == 0) {
			hp = strtol(property->value, NULL, 10);
		}
	}
	mtx_unlock(&hyruleMutex);

	return (hp > 0);
}

/**
 * hyruleReset - Restore all properties to their default values.
 */
void
hyruleReset(void)
{
	struct hyruleProperty *property;

	/* 
	 * Reset all property values to their defined defaults. 
	 * We must hold the structural lock during iteration.
	 */
	mtx_lock(&hyruleMutex);
	LIST_FOREACH(property, &propertyList, next) {
		strlcpy(property->value, property->defaultValue, sizeof(property->value));
	}
	mtx_unlock(&hyruleMutex);

	/* Reset global states. */
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
hyruleRead(struct cdev *dev, struct uio *userIo, int ioflag)
{
	struct hyruleProperty *property = dev->si_drv1;
	int error;
	size_t len;

	/* 
	 * Each character device stores a pointer to its corresponding property 
	 * structure in si_drv1. If this is NULL, the device was not set up correctly.
	 */
	if (property == NULL)
		return (ENXIO);

	/* 
	 * Acquire a shared lock to protect the property's value during the read. 
	 * Multiple processes can read the same property concurrently.
	 */
	sx_slock(&hyruleSharedExclusion);

	/* 
	 * Check if the 'game engine' is active. If not, certain nodes return status messages
	 * instead of their stored values.
	 */
	if (!hyruleIsActive() && 
	    strcmp(property->name, "console/power") != 0 && 
	    strcmp(property->name, "console/reset") != 0 &&
	    strcmp(property->name, "console/cartridge") != 0 &&
	    strcmp(property->name, "help") != 0) {
		const char *msg;
		if (hyrulePower == 0)
			msg = "POWER OFF\n";
		else
			msg = "GAME OVER\n";
		
		len = strlen(msg);
		/* Return EOF if the user has already read the whole message. */
		if (userIo->uio_offset >= len) {
			sx_sunlock(&hyruleSharedExclusion);
			return (0);
		}
		/* Move data from kernel space to user space via the uio framework. */
		error = uiomove(__DECONST(char *, msg) + userIo->uio_offset, len - userIo->uio_offset, userIo);
		sx_sunlock(&hyruleSharedExclusion);
		return (error);
	}

	/* 
	 * Calculate the length of the stored property value and check seek offset.
	 */
	len = strnlen(property->value, sizeof(property->value));
	if (userIo->uio_offset >= len) {
		sx_sunlock(&hyruleSharedExclusion);
		return (0);
	}
	/* Perform the actual data copy to the user's buffer. */
	error = uiomove(property->value + userIo->uio_offset, len - userIo->uio_offset, userIo);
	
	/* Unlock and return. */
	sx_sunlock(&hyruleSharedExclusion);
	return (error);
}

/**
 * hyruleCpuRead - Specialized read handler for /dev/hyrule/console/cpu.
 */
static int
hyruleCpuRead(struct cdev *dev, struct uio *userIo, int ioflag)
{
	struct sbuf *stringBuffer;
	long *cpuTimes;
	int error;
	size_t len;
	size_t timesSize;

	/* 
	 * Allocate memory to store CPU ticks for all processors. 
	 * mp_ncpus is a kernel global indicating the number of active CPUs.
	 */
	timesSize = mp_ncpus * CPUSTATES * sizeof(long);
	cpuTimes = malloc(timesSize, M_DEVBUF, M_WAITOK | M_ZERO);

	/* 
	 * kern.cp_times provides the cumulative execution ticks for each CPU 
	 * across different states (User, Nice, Sys, Interrupt, Idle).
	 */
	if (kernel_sysctlbyname(curthread, "kern.cp_times", cpuTimes, &timesSize, NULL, 0, NULL, 0) != 0) {
		memset(cpuTimes, 0, timesSize);
	}

	/* Initialize a dynamic string buffer for building the JSON output. */
	stringBuffer = sbuf_new_auto();
	if (stringBuffer == NULL) {
		free(cpuTimes, M_DEVBUF);
		return (ENOMEM);
	}

	sbuf_printf(stringBuffer, "{\n");
	sbuf_printf(stringBuffer, "  \"ncpus\": %d,\n", mp_ncpus);
	
	/* 
	 * Report the status of the background temperature driver loading.
	 */
	sbuf_printf(stringBuffer, "  \"drivers\": {\n");
	sbuf_printf(stringBuffer, "    \"coretemp\": \"%s\",\n", 
	    (hyruleCoretempResult == 0 || hyruleCoretempResult == EEXIST) ? "loaded" : 
	    (hyruleCoretempResult == -1) ? "pending" : "not found");
	sbuf_printf(stringBuffer, "    \"amdtemp\": \"%s\"\n",
	    (hyruleAmdtempResult == 0 || hyruleAmdtempResult == EEXIST) ? "loaded" :
	    (hyruleAmdtempResult == -1) ? "pending" : "not found");
	sbuf_printf(stringBuffer, "  },\n");

	sbuf_printf(stringBuffer, "  \"cpus\": [\n");

	for (int i = 0; i < mp_ncpus; i++) {
		int temperatureValue = 0;
		size_t temperatureSize = sizeof(temperatureValue);
		char sysctlName[64];

		/* 
		 * Attempt to fetch the temperature from the coretemp/amdtemp sysctls.
		 * Values are returned in deci-Kelvin (K * 10).
		 */
		snprintf(sysctlName, sizeof(sysctlName), "dev.cpu.%d.temperature", i);
		error = kernel_sysctlbyname(curthread, sysctlName, &temperatureValue, &temperatureSize, NULL, 0, NULL, 0);

		sbuf_printf(stringBuffer, "    {\n      \"id\": %d,\n", i);
		if (error == 0) {
			/* Convert deci-Kelvin to Celsius with one decimal place. */
			int val = temperatureValue - 2731;
			int whole = val / 10;
			int frac = val % 10;
			if (frac < 0) frac = -frac;
			sbuf_printf(stringBuffer, "      \"temperature_c\": %d.%d,\n", whole, frac);
		} else {
			sbuf_printf(stringBuffer, "      \"temperature_c\": null,\n");
			sbuf_printf(stringBuffer, "      \"temperature_status\": \"%s\",\n",
			    (error == ENOENT) ? "no sysctl" : "error");
		}

		/* 
		 * Add the CPU execution statistics.
		 */
		sbuf_printf(stringBuffer, "      \"stats\": {\n");
		sbuf_printf(stringBuffer, "        \"user\": %ld,\n", cpuTimes[i * CPUSTATES + CP_USER]);
		sbuf_printf(stringBuffer, "        \"nice\": %ld,\n", cpuTimes[i * CPUSTATES + CP_NICE]);
		sbuf_printf(stringBuffer, "        \"sys\": %ld,\n", cpuTimes[i * CPUSTATES + CP_SYS]);
		sbuf_printf(stringBuffer, "        \"intr\": %ld,\n", cpuTimes[i * CPUSTATES + CP_INTR]);
		sbuf_printf(stringBuffer, "        \"idle\": %ld\n", cpuTimes[i * CPUSTATES + CP_IDLE]);
		sbuf_printf(stringBuffer, "      }\n");
		
		sbuf_printf(stringBuffer, "    }%s\n", (i == mp_ncpus - 1) ? "" : ",");
	}

	sbuf_printf(stringBuffer, "  ]\n}\n");
	sbuf_finish(stringBuffer);

	len = sbuf_len(stringBuffer);
	/* If the user seeked past the end, return EOF. */
	if (userIo->uio_offset >= len) {
		error = 0;
		goto out;
	}

	/* Copy the generated JSON to userspace. */
	error = uiomove(sbuf_data(stringBuffer) + userIo->uio_offset, len - userIo->uio_offset, userIo);

out:
	/* Clean up resources. */
	sbuf_delete(stringBuffer);
	free(cpuTimes, M_DEVBUF);
	return (error);
}

/**
 * hyruleUpdatePropState - Process side-effects of property changes.
 */
static void
hyruleUpdatePropState(struct hyruleProperty *property, int loading)
{
	/* 
	 * If the property being changed is the power state, we may need to 
	 * trigger a system reset or handle cartridge dust simulation.
	 */
	if (strcmp(property->name, "console/power") == 0) {
		int newPowerState = strtol(property->value, NULL, 10);
		if (newPowerState != hyrulePower) {
			if (newPowerState == 1 && !loading) {
				/* 80% chance of failure if the cartridge is dusty. */
				if (hyruleCartridge == 0 && (arc4random() % 100) < 80) {
					printf("[HYRULE] Power-on failed. Red light blinking. Cartridge dusty?\n");
					strlcpy(property->value, "0\n", sizeof(property->value));
					return;
				}
				hyruleReset();
			}
			hyrulePower = newPowerState;
			printf("[HYRULE] Power set to %d\n", hyrulePower);
			/* 50% chance the cartridge gets dusty when powering off. */
			if (hyrulePower == 0 && !loading) {
				if ((arc4random() % 100) < 50) {
					hyruleCartridge = 0;
					struct hyruleProperty *cartridgeProperty;
					mtx_lock(&hyruleMutex);
					LIST_FOREACH(cartridgeProperty, &propertyList, next) {
						if (strcmp(cartridgeProperty->name, "console/cartridge") == 0) {
							strlcpy(cartridgeProperty->value, "dusty\n", sizeof(cartridgeProperty->value));
							break;
						}
					}
					mtx_unlock(&hyruleMutex);
					printf("[HYRULE] Cartridge is now dusty after power cycle.\n");
				}
			}
		}
	} else if (strcmp(property->name, "console/reset") == 0) {
		/* Triggering a reset restores all values. */
		if (strtol(property->value, NULL, 10) == 1) {
			hyruleReset();
			strlcpy(property->value, "0\n", sizeof(property->value));
		}
	} else if (strcmp(property->name, "console/cartridge") == 0) {
		/* Blowing on the cartridge cleans it. */
		if (strncmp(property->value, "blow", 4) == 0 || strncmp(property->value, "clean", 5) == 0) {
			hyruleCartridge = 1;
			if (strncmp(property->value, "blow", 4) == 0)
				strlcpy(property->value, "clean\n", sizeof(property->value));
			printf("[HYRULE] Cartridge is now clean and ready to play!\n");
		} else {
			hyruleCartridge = 0;
			strlcpy(property->value, "dusty\n", sizeof(property->value));
		}
	} else if (strcmp(property->name, "characters/link/stats/health") == 0) {
		long healthPoints = strtol(property->value, NULL, 10);
		if (healthPoints <= 0) {
			printf("[HYRULE] Link has no life left! GAME OVER.\n");
		}
	} else if (strcmp(property->name, "characters/link/status/invincible") == 0) {
		int newInvincibilityState = strtol(property->value, NULL, 10);
		if (newInvincibilityState != hyruleInvincible) {
			hyruleInvincible = newInvincibilityState;
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
	struct hyruleProperty *nodeToRemove = NULL;

	/* 
	 * We use an exclusive lock because we are potentially modifying the 
	 * property nodes (adding or removing).
	 */
	sx_xlock(&hyruleSharedExclusion);
	if (hyruleInvincible) {
		if (invinciblePropertyNode == NULL) {
			addHyrulePropertyNode("characters/link/status/invincible", "1\n");
			struct hyruleProperty *property;
			mtx_lock(&hyruleMutex);
			LIST_FOREACH(property, &propertyList, next) {
				if (strcmp(property->name, "characters/link/status/invincible") == 0) {
					invinciblePropertyNode = property;
					break;
				}
			}
			mtx_unlock(&hyruleMutex);
		}
	} else {
		if (invinciblePropertyNode != NULL) {
			nodeToRemove = invinciblePropertyNode;
			invinciblePropertyNode = NULL;
		}
	}
	sx_xunlock(&hyruleSharedExclusion);

	/* 
	 * Perform the removal outside of the lock to avoid potential deadlocks 
	 * during device destruction.
	 */
	if (nodeToRemove != NULL)
		removeHyrulePropertyNode(nodeToRemove);
}

/**
 * hyruleLoadModulesThread - Background thread for sideloading modules.
 */
static void
hyruleLoadModulesThread(void *arg)
{
	int fileId;
	/* 
	 * Dynamically attempt to load the required temperature drivers. 
	 * kern_kldload handles the kernel linker operations.
	 */
	hyruleCoretempResult = kern_kldload(curthread, "coretemp", &fileId);
	hyruleAmdtempResult = kern_kldload(curthread, "amdtemp", &fileId);
	kproc_exit(0);
}

void
hyruleUpdateStatusNodes(void)
{
	taskqueue_enqueue(taskqueue_thread, &statusUpdateTaskQueueItem);
}

/**
 * hyruleWrite - Generic write handler for property nodes.
 */
int
hyruleWrite(struct cdev *dev, struct uio *userIo, int ioflag)
{
	struct hyruleProperty *property = dev->si_drv1;
	int error;
	size_t length;

	/* 
	 * Retrieve the property associated with this device node. 
	 * If it's missing, the device is invalid.
	 */
	if (property == NULL)
		return (ENXIO);

	/* 
	 * We use an exclusive lock (sx_xlock) because we are modifying 
	 * the property's value.
	 */
	sx_xlock(&hyruleSharedExclusion);

	/* 
	 * If the system is powered off, we restrict modifications to 
	 * essential 'control' nodes like power, cartridge, and reset.
	 */
	if (!hyrulePower && 
	    strcmp(property->name, "console/power") != 0 &&
	    strcmp(property->name, "console/cartridge") != 0 &&
	    strcmp(property->name, "console/reset") != 0) {
		sx_xunlock(&hyruleSharedExclusion);
		return (EACCES);
	}

	/* Check if the user's seek position is within our buffer limits. */
	if (userIo->uio_offset >= sizeof(property->value) - 1) {
		sx_xunlock(&hyruleSharedExclusion);
		return (EFBIG);
	}
	
	/* Calculate how much data we can safely accept. */
	length = MIN(userIo->uio_resid, sizeof(property->value) - 1 - userIo->uio_offset);
	/* Transfer data from userspace to our kernel buffer. */
	error = uiomove(property->value + userIo->uio_offset, length, userIo);
	/* Ensure the resulting string is null-terminated. */
	property->value[sizeof(property->value) - 1] = '\0';

	if (error != 0) {
		sx_xunlock(&hyruleSharedExclusion);
		return (error);
	}

	/* Trigger any side-effects associated with this property change. */
	hyruleUpdatePropState(property, 0);

	/* Release the lock and return success. */
	sx_xunlock(&hyruleSharedExclusion);
	return (0);
}

/**
 * hyruleValidateSave - Parser validation.
 */
static int
hyruleValidateSave(const char *buffer, size_t totalLength)
{
	const char *bufferPointer = buffer;
	const char *bufferEnd = buffer + totalLength;

	/* 
	 * Iterate through the buffer, parsing each 'PROP:' entry.
	 */
	while (bufferPointer < bufferEnd) {
		/* Skip whitespace and newlines. */
		while (bufferPointer < bufferEnd && (*bufferPointer == '\n' || *bufferPointer == '\r' || *bufferPointer == ' ')) bufferPointer++;
		if (bufferPointer >= bufferEnd) break;

		/* Each entry must start with 'PROP:'. */
		if (bufferEnd - bufferPointer < 5 || strncmp(bufferPointer, "PROP:", 5) != 0) return (EINVAL);
		bufferPointer += 5;
		const char *nameStart = bufferPointer;
		const char *lineEnd = memchr(bufferPointer, '\n', bufferEnd - bufferPointer);
		if (!lineEnd) return (EINVAL);
		
		size_t nameLength = lineEnd - nameStart;
		if (nameLength > 0 && nameStart[nameLength-1] == '\r') nameLength--;
		
		char tmpName[256];
		if (nameLength >= sizeof(tmpName)) return (EINVAL);
		memcpy(tmpName, nameStart, nameLength);
		tmpName[nameLength] = '\0';

		/* Verify that the property named in the save file actually exists in our system. */
		int found = 0;
		if (strcmp(tmpName, "world/map_config") == 0 ||
		    strcmp(tmpName, "characters/link/status/invincible") == 0) {
			found = 1;
		} else {
			struct hyruleProperty *property;
			mtx_lock(&hyruleMutex);
			LIST_FOREACH(property, &propertyList, next) {
				if (strcmp(property->name, tmpName) == 0) {
					found = 1;
					break;
				}
			}
			mtx_unlock(&hyruleMutex);
		}
		if (!found) return (ENOENT);

		bufferPointer = lineEnd + 1;

		/* Each entry must have a 'SIZE:' field. */
		if (bufferEnd - bufferPointer < 5 || strncmp(bufferPointer, "SIZE:", 5) != 0) return (EINVAL);
		bufferPointer += 5;
		lineEnd = memchr(bufferPointer, '\n', bufferEnd - bufferPointer);
		if (!lineEnd) return (EINVAL);

		size_t valueLength = 0;
		for (const char *c = bufferPointer; c < lineEnd; c++) {
			if (*c == '\r') continue;
			if (*c < '0' || *c > '9') return (EINVAL);
			valueLength = valueLength * 10 + (*c - '0');
		}
		/* Sanity check on the value size. */
		if (valueLength > 1024) return (EFBIG);

		bufferPointer = lineEnd + 1;
		/* Ensure the buffer contains the full value as described by SIZE. */
		if (bufferEnd - bufferPointer < valueLength) return (EINVAL);

		bufferPointer += valueLength;
		if (bufferPointer < bufferEnd && *bufferPointer != '\n' && *bufferPointer != '\r') return (EINVAL);
	}
	return (0);
}

/**
 * hyruleSaveRead - Handler for /dev/hyrule/game/save.
 */
static int
hyruleSaveRead(struct cdev *dev, struct uio *userIo, int ioflag)
{
	char *buffer;
	int length = 0, error;
	struct hyruleProperty *property;

	/* 
	 * Allocate a temporary buffer to construct the save file content.
	 */
	buffer = malloc(16384, M_DEVBUF, M_WAITOK | M_ZERO);

	/* 
	 * Use a shared lock to read property values and a mutex for list traversal.
	 */
	sx_slock(&hyruleSharedExclusion);
	mtx_lock(&hyruleMutex);
	LIST_FOREACH(property, &propertyList, next) {
		/* Skip ephemeral or control nodes that shouldn't be saved. */
		if (strcmp(property->name, "help") == 0 ||
		    strncmp(property->name, "map/", 4) == 0 ||
		    strncmp(property->name, "console/controller/", 19) == 0 ||
		    strcmp(property->name, "console/reset") == 0 ||
		    strcmp(property->name, "game/save") == 0 ||
		    strcmp(property->name, "game/load") == 0)
			continue;

		if (strcmp(property->name, "world/map_config") == 0) {
			char mapConfigBuffer[128];
			hyruleMapGetConfig(mapConfigBuffer, sizeof(mapConfigBuffer));
			length += snprintf(buffer + length, 16384 - length, "PROP:%s\nSIZE:%zu\n%s\n\n",
			    property->name, strlen(mapConfigBuffer), mapConfigBuffer);
		} else {
			length += snprintf(buffer + length, 16384 - length, "PROP:%s\nSIZE:%zu\n%s\n\n",
			    property->name, strlen(property->value), property->value);
		}
	}
	mtx_unlock(&hyruleMutex);
	sx_sunlock(&hyruleSharedExclusion);

	/* Return EOF if we've read everything. */
	if (userIo->uio_offset >= length) {
		free(buffer, M_DEVBUF);
		return (0);
	}
	/* Copy the constructed save data to the user. */
	error = uiomove(buffer + userIo->uio_offset, length - userIo->uio_offset, userIo);
	free(buffer, M_DEVBUF);
	return (error);
}

/**
 * hyruleLoadWrite - Handler for /dev/hyrule/game/load.
 */
static int
hyruleLoadWrite(struct cdev *dev, struct uio *userIo, int ioflag)
{
	char *buffer, *bufferPointer, *line, *name, *valueString;
	size_t length, valueLength;
	int error;

	/* Check the size of the input data. */
	length = userIo->uio_resid;
	if (length > 16384) return (EFBIG);

	/* Allocate a buffer and copy the save data from userspace. */
	buffer = malloc(length + 1, M_DEVBUF, M_WAITOK | M_ZERO);
	error = uiomove(buffer, length, userIo);
	if (error) {
		free(buffer, M_DEVBUF);
		return (error);
	}
	buffer[length] = '\0';

	/* Validate the format and content of the save data. */
	error = hyruleValidateSave(buffer, length);
	if (error != 0) {
		printf("[HYRULE] Load rejected: Invalid save format or property (error=%d)\n", error);
		free(buffer, M_DEVBUF);
		return (error);
	}

	/* Use an exclusive lock to update the property values. */
	sx_xlock(&hyruleSharedExclusion);
	bufferPointer = buffer;
	while (bufferPointer && *bufferPointer) {
		/* Skip whitespace. */
		while (*bufferPointer == '\n' || *bufferPointer == '\r' || *bufferPointer == ' ') bufferPointer++;
		if (*bufferPointer == '\0') break;

		/* Parse each 'PROP:' entry and update the corresponding property. */
		line = strsep(&bufferPointer, "\n");
		if (line && strncmp(line, "PROP:", 5) == 0) {
			name = line + 5;
			size_t nameLength = strlen(name);
			if (nameLength > 0 && name[nameLength-1] == '\r') name[nameLength-1] = '\0';

			line = strsep(&bufferPointer, "\n");
			if (line && strncmp(line, "SIZE:", 5) == 0) {
				valueLength = strtoul(line + 5, NULL, 10);
				valueString = bufferPointer;
				if (valueString && valueLength <= strlen(valueString)) {
					bufferPointer += valueLength;
					char savedChar = *bufferPointer;
					*bufferPointer = '\0';

					if (strcmp(name, "world/map_config") == 0) {
						hyruleMapSetConfig(valueString, valueLength);
					} else if (strcmp(name, "characters/link/status/invincible") == 0) {
						hyruleInvincible = strtol(valueString, NULL, 10);
						hyruleUpdateStatusNodes();
					} else {
						struct hyruleProperty *property;
						mtx_lock(&hyruleMutex);
						LIST_FOREACH(property, &propertyList, next) {
							if (strcmp(property->name, name) == 0) {
								strlcpy(property->value, valueString, sizeof(property->value));
								/* Apply side-effects (loading=1 indicates we are in a load state). */
								hyruleUpdatePropState(property, 1);
								break;
							}
						}
						mtx_unlock(&hyruleMutex);
					}
					*bufferPointer = savedChar;
				}
			}
		}
	}
	sx_xunlock(&hyruleSharedExclusion);

	free(buffer, M_DEVBUF);
	return (0);
}

/**
 * addHyrulePropertyNodeCustom - Create a new hierarchical property node.
 */
int
addHyrulePropertyNodeCustom(const char *path, const char *initialVal, struct cdevsw *characterDeviceSwitch)
{
	struct hyruleProperty *property;
	struct make_dev_args deviceArgs;
	int error;

	/* Basic validation of input parameters. */
	if (path == NULL || initialVal == NULL)
		return (EINVAL);

	/* 
	 * Allocate memory for the property structure. M_WAITOK ensures the 
	 * allocation will sleep until memory is available.
	 */
	property = malloc(sizeof(*property), M_DEVBUF, M_WAITOK | M_ZERO);
	if (property == NULL)
		return (ENOMEM);

	/* Set up the property's initial state. */
	strlcpy(property->name, path, sizeof(property->name));
	strlcpy(property->value, initialVal, sizeof(property->value));
	strlcpy(property->defaultValue, initialVal, sizeof(property->defaultValue));

	/* Initialize arguments for character device creation. */
	make_dev_args_init(&deviceArgs);
	deviceArgs.mda_flags = MAKEDEV_CHECKNAME | MAKEDEV_WAITOK;
	deviceArgs.mda_devsw = characterDeviceSwitch;
	deviceArgs.mda_uid = UID_ROOT;
	deviceArgs.mda_gid = GID_WHEEL;
	deviceArgs.mda_mode = 0666;
	deviceArgs.mda_si_drv1 = property; /* Attach our property structure to the device. */

	/* Create the device node under /dev/hyrule/. */
	error = make_dev_s(&deviceArgs, &property->characterDevice, "hyrule/%s", property->name);
	if (error != 0) {
		printf("[HYRULE] Failed to create /dev/hyrule/%s (error=%d)\n", property->name, error);
		free(property, M_DEVBUF);
		return (error);
	}

	/* Insert the new property into the global list. */
	mtx_lock(&hyruleMutex);
	LIST_INSERT_HEAD(&propertyList, property, next);
	mtx_unlock(&hyruleMutex);
	return (0);
}

/**
 * removeHyrulePropertyNode - Destroy a property node and its /dev entry.
 */
void
removeHyrulePropertyNode(struct hyruleProperty *property)
{
	/* Safety check for NULL pointers. */
	if (property == NULL)
		return;

	/* Remove the property from the global list first to prevent new accesses. */
	mtx_lock(&hyruleMutex);
	LIST_REMOVE(property, next);
	mtx_unlock(&hyruleMutex);

	/* Destroy the character device entry in /dev/. */
	if (property->characterDevice)
		destroy_dev(property->characterDevice);
	
	/* Free the memory associated with the property. */
	free(property, M_DEVBUF);
}

/**
 * addHyrulePropertyNode - Shortcut to create a standard property node.
 */
int
addHyrulePropertyNode(const char *path, const char *initialVal)
{
	/* Wraps the custom call using our default character device switch. */
	return addHyrulePropertyNodeCustom(path, initialVal, &hyruleCharacterDeviceSwitch);
}

/**
 * hyruleLoader - Module event handler.
 * 
 * This function handles kernel events like loading, unloading, and system shutdown.
 */
static int
hyruleLoader(struct module *mod, int cmd, void *arg)
{
	int error = 0;
	struct hyruleProperty *property;

	switch (cmd) {
	case MOD_LOAD:
		/* Initialize global synchronization primitives. */
		mtx_init(&hyruleMutex, "hyrule list lock", NULL, MTX_DEF);
		sx_init(&hyruleSharedExclusion, "hyrule data lock");
		
		/* Initialize subsystems. */
		hyruleMapInit();
		hyruleInputInit();

		/* Set up the background task for dynamic status nodes. */
		TASK_INIT(&statusUpdateTaskQueueItem, 0, hyruleUpdateStatusNodesTask, NULL);

		/* 
		 * Create the initial set of property nodes under /dev/hyrule/.
		 * We use helper functions to handle the boilerplate of device creation.
		 */
		error = addHyrulePropertyNode("console/power", "1\n");
		if (error) goto fail;
		error = addHyrulePropertyNode("console/reset", "0\n");
		if (error) goto fail;
		error = addHyrulePropertyNode("console/cartridge", "dusty\n");
		if (error) goto fail;
		error = addHyrulePropertyNodeCustom("console/cpu", "", &hyruleCpuCharacterDeviceSwitch);
		if (error) goto fail;

		error = addHyrulePropertyNode("help", 
		    "Welcome to the Hyrule Kernel Module!\n\n"
		    "Map display: cat /dev/hyrule/map/view\n"
		    "World config: /dev/hyrule/world/map_config\n"
		    "Move Link: echo 'up' > /dev/hyrule/characters/link/location/controller\n\n"
		    "Be careful, it's dangerous to go alone!\n");
		if (error) goto fail;

		error = addHyrulePropertyNodeCustom("map/view", "", &hyruleMapCharacterDeviceSwitch);
		if (error) goto fail;
		error = addHyrulePropertyNodeCustom("world/map_config", "", &hyruleMapConfigCharacterDeviceSwitch);
		if (error) goto fail;
		error = addHyrulePropertyNodeCustom("characters/link/location/controller", "", &hyruleControllerCharacterDeviceSwitch);
		if (error) goto fail;
		
		/* Populate initial controller state nodes. */
		hyruleUpdateControllerNodes();

		/* Set up persistence (save/load) nodes. */
		error = addHyrulePropertyNodeCustom("game/save", "", &hyruleSaveCharacterDeviceSwitch);
		if (error) goto fail;
		error = addHyrulePropertyNodeCustom("game/load", "", &hyruleLoadCharacterDeviceSwitch);
		if (error) goto fail;

		/* Start a background thread to sideload temperature drivers. */
		kproc_create(hyruleLoadModulesThread, NULL, NULL, 0, 0, "hyrule_loader");

		/* Add remaining game state properties. */
		error = addHyrulePropertyNode("characters/link/stats/health", "3\n");
		if (error) goto fail;
		error = addHyrulePropertyNode("characters/link/stats/stamina", "100\n");
		if (error) goto fail;
		error = addHyrulePropertyNode("characters/link/stats/rupees", "0\n");
		if (error) goto fail;
		error = addHyrulePropertyNode("characters/link/location/x", "0\n");
		if (error) goto fail;
		error = addHyrulePropertyNode("characters/link/location/y", "0\n");
		if (error) goto fail;

		error = addHyrulePropertyNode("characters/link/items/sword", "None\n");
		if (error) goto fail;
		error = addHyrulePropertyNode("characters/link/stats/sword_level", "0\n");
		if (error) goto fail;

		error = addHyrulePropertyNode("characters/zelda/stats/health", "100\n");
		if (error) goto fail;
		error = addHyrulePropertyNode("characters/ganon/stats/health", "200\n");
		if (error) goto fail;

		/* Refresh the dynamic status nodes. */
		hyruleUpdateStatusNodes();

		printf("[HYRULE] Hyrule is now mapped to /dev/hyrule/\n");
		break;

	case MOD_UNLOAD:
		/* Clean up background tasks and wait for them to finish. */
		taskqueue_drain(taskqueue_thread, &statusUpdateTaskQueueItem);
		hyruleMapDrain();
		hyruleInputDrain();

		/* 
		 * Iterate through and destroy all remaining property nodes. 
		 * We hold the lock while popping items from the list.
		 */
		while (1) {
			mtx_lock(&hyruleMutex);
			property = LIST_FIRST(&propertyList);
			if (property == NULL) {
				mtx_unlock(&hyruleMutex);
				break;
			}
			LIST_REMOVE(property, next);
			mtx_unlock(&hyruleMutex);

			/* Destroy the character device and free our structure. */
			destroy_dev(property->characterDevice);
			free(property, M_DEVBUF);
		}
		
		/* Final cleanup of locks. */
		mtx_destroy(&hyruleMutex);
		sx_destroy(&hyruleSharedExclusion);
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
	/* Error path: cleanup any partially initialized state. */
	taskqueue_drain(taskqueue_thread, &statusUpdateTaskQueueItem);
	hyruleMapDrain();
	hyruleInputDrain();
	while (1) {
		mtx_lock(&hyruleMutex);
		property = LIST_FIRST(&propertyList);
		if (property == NULL) {
			mtx_unlock(&hyruleMutex);
			break;
		}
		LIST_REMOVE(property, next);
		mtx_unlock(&hyruleMutex);
		if (property->characterDevice)
			destroy_dev(property->characterDevice);
		free(property, M_DEVBUF);
	}
	mtx_destroy(&hyruleMutex);
	sx_destroy(&hyruleSharedExclusion);
	return (error);
}

static moduledata_t hyruleMod = {
	"hyrule",
	hyruleLoader,
	NULL
};

DECLARE_MODULE(hyrule, hyruleMod, SI_SUB_DRIVERS, SI_ORDER_MIDDLE);
MODULE_VERSION(hyrule, 1);
