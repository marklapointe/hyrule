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

#ifndef _HYRULE_H_
#define _HYRULE_H_

#include <sys/param.h>
#include <sys/conf.h>
#include <sys/queue.h>
#include <sys/sx.h>
#include <sys/mutex.h>
#include <sys/uio.h>
#include <sys/poll.h>
#include <sys/event.h>
#include <sys/bio.h>
#include <vm/vm.h>
#include <vm/pmap.h>

#define MAP_SIZE 10

/* 
 * Variables (Global state and locks)
 * 
 * hyruleMutex: Fast leaf mutex for list integrity.
 * hyruleSharedExclusion: Sleepable SX lock for data contents and I/O.
 * propertyList: Head of the linked list of all properties.
 */
struct hyruleProperty;
extern struct mtx hyruleMutex;
extern struct sx hyruleSharedExclusion;
extern LIST_HEAD(propertyHead, hyruleProperty) propertyList;
extern int hyrulePower;
extern int hyruleCartridge;
extern int hyruleInvincible;

/* 
 * Shared device operations (Handlers).
 * These follow the FreeBSD characterDeviceSwitch (character device switch) interface.
 */
extern struct cdevsw hyruleMapCharacterDeviceSwitch;
extern struct cdevsw hyruleMapConfigCharacterDeviceSwitch;
extern struct cdevsw hyruleControllerCharacterDeviceSwitch;
extern struct cdevsw hyruleSaveCharacterDeviceSwitch;
extern struct cdevsw hyruleLoadCharacterDeviceSwitch;
extern struct cdevsw hyruleLocalCharacterDeviceSwitch;

/*
 * Structs/Types
 */

/**
 * struct hyruleProperty - Represents a single property/node in the system.
 *
 * Each instance of this structure corresponds to a character device under /dev/hyrule/.
 * @characterDevice: Pointer to the kernel's character device structure.
 * @name: The path of the property (e.g., "characters/link/stats/health").
 * @value: The current value of the property, stored as a string.
 * @defaultValue: The value this property returns to after a reset.
 * @next: Linked list pointers for the global property list.
 */
struct hyruleProperty {
	struct cdev *characterDevice;
	char name[256];
	char value[1024]; /* Enough for 10x10 map and strings */
	char defaultValue[1024];
	LIST_ENTRY(hyruleProperty) next;
};

/* 
 * Methods/Functions (Prototypes)
 */

/* Shared device operations (Handlers) */
d_open_t  hyruleOpen;
d_close_t hyruleClose;
d_read_t  hyruleRead;
d_write_t hyruleWrite;
d_ioctl_t hyruleIoctl;
d_poll_t  hyrulePoll;
d_mmap_t  hyruleMmap;
d_kqfilter_t hyruleKqfilter;
d_strategy_t hyruleStrategy;
d_fdopen_t hyruleFdopen;
d_mmap_single_t hyruleMmapSingle;
d_purge_t hyrulePurge;

/* Helper functions in hyrule.c */
int addHyrulePropertyNode(const char *path, const char *initialVal);
int addHyrulePropertyNodeCustom(const char *path, const char *initialVal, struct cdevsw *deviceSwitch);
void removeHyrulePropertyNode(struct hyruleProperty *property);
void hyruleReset(void);
int hyruleIsActive(void);
int hyruleGetPropInt(const char *name, int defaultVal);
void hyruleSetPropInt(const char *name, int val);

/* Map logic in hyrule_map.c */
void hyruleMapInit(void);
int hyruleMapIsAccessible(int x, int y);
void hyruleMapGetConfig(char *buf, size_t size);
void hyruleMapSetConfig(const char *input, size_t len);
void hyruleUpdateControllerNodes(void);
void hyruleUpdateLocalNodes(void);
void hyruleUpdateStatusNodes(void);
void hyruleMapDrain(void);
void hyruleInputDrain(void);

/* Input logic in hyrule_input.c */
void hyruleInputInit(void);

#endif /* _HYRULE_H_ */
