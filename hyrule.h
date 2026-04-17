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
 * Hyrule Kernel Module Structure
 */

struct hyrule_prop {
	struct cdev *cdev;
	char name[256];
	char value[1024]; /* Enough for 10x10 map and strings */
	LIST_ENTRY(hyrule_prop) next;
};

/* Global locks and list defined in hyrule.c */
extern struct mtx hyrule_mtx;
extern struct sx hyrule_sx;
extern LIST_HEAD(prop_head, hyrule_prop) prop_list;

/* Shared device operations */
d_open_t  hyrule_open;
d_close_t hyrule_close;
d_read_t  hyrule_read;
d_write_t hyrule_write;
d_ioctl_t hyrule_ioctl;
d_poll_t  hyrule_poll;
d_mmap_t  hyrule_mmap;
d_kqfilter_t hyrule_kqfilter;
d_strategy_t hyrule_strategy;
d_fdopen_t hyrule_fdopen;
d_mmap_single_t hyrule_mmap_single;
d_purge_t hyrule_purge;

/* Helper functions in hyrule.c */
int add_hyrule_node(const char *path, const char *initial_val);
int add_hyrule_node_custom(const char *path, const char *initial_val, struct cdevsw *sw);

/* Map logic in hyrule_map.c */
extern struct cdevsw hyrule_map_cdevsw;
extern struct cdevsw hyrule_map_config_cdevsw;
extern struct cdevsw hyrule_move_cdevsw;

void hyrule_map_init(void);

#endif /* _HYRULE_H_ */
