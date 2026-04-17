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
#include <sys/systm.h>
#include <sys/ctype.h>

/* Map State */
static char world_map[MAP_SIZE][MAP_SIZE];
static int link_x = 0;
static int link_y = 0;

/* Symbols for display */
static char
get_map_symbol(char type)
{
	switch (tolower(type)) {
	case 'f': return ('.'); /* Field */
	case 'w': return ('T'); /* Woods */
	case 'e': return (' '); /* Empty */
	case 'd': return ('D'); /* Dungeon */
	case 'a': return ('F'); /* Fairy */
	default:  return ('?');
	}
}

static int
is_accessible(int x, int y)
{
	if (x < 0 || x >= MAP_SIZE || y < 0 || y >= MAP_SIZE)
		return (0);
	/* Lowercase is accessible, uppercase is blocked */
	return (islower(world_map[y][x]));
}

/* Display /dev/hyrule/map */
static int
hyrule_map_read(struct cdev *dev, struct uio *uio, int ioflag)
{
	char buf[1024];
	int pos = 0;
	int x, y;

	sx_slock(&hyrule_sx);

	pos += snprintf(buf + pos, sizeof(buf) - pos, "--- Hyrule Map ---\n");
	for (y = 0; y < MAP_SIZE; y++) {
		for (x = 0; x < MAP_SIZE; x++) {
			if (x == link_x && y == link_y)
				pos += snprintf(buf + pos, sizeof(buf) - pos, "L");
			else
				pos += snprintf(buf + pos, sizeof(buf) - pos, "%c", get_map_symbol(world_map[y][x]));
		}
		pos += snprintf(buf + pos, sizeof(buf) - pos, "\n");
	}
	pos += snprintf(buf + pos, sizeof(buf) - pos, "Link at: (%d, %d)\n", link_x, link_y);

	if (uio->uio_offset >= pos) {
		sx_sunlock(&hyrule_sx);
		return (0);
	}

	int error = uiomove(buf + uio->uio_offset, pos - uio->uio_offset, uio);
	sx_sunlock(&hyrule_sx);
	return (error);
}

/* Read/Write /dev/hyrule/world/map_config */
static int
hyrule_map_config_read(struct cdev *dev, struct uio *uio, int ioflag)
{
	char buf[128];
	int x, y, pos = 0;

	sx_slock(&hyrule_sx);
	for (y = 0; y < MAP_SIZE; y++) {
		for (x = 0; x < MAP_SIZE; x++) {
			buf[pos++] = world_map[y][x];
		}
		buf[pos++] = '\n';
	}
	buf[pos] = '\0';

	if (uio->uio_offset >= pos) {
		sx_sunlock(&hyrule_sx);
		return (0);
	}

	int error = uiomove(buf + uio->uio_offset, pos - uio->uio_offset, uio);
	sx_sunlock(&hyrule_sx);
	return (error);
}

static int
hyrule_map_config_write(struct cdev *dev, struct uio *uio, int ioflag)
{
	char input[256];
	int error, len, x = 0, y = 0, i;

	sx_xlock(&hyrule_sx);
	len = MIN(uio->uio_resid, sizeof(input) - 1);
	error = uiomove(input, len, uio);
	if (error) {
		sx_xunlock(&hyrule_sx);
		return (error);
	}
	input[len] = '\0';

	for (i = 0; i < len && y < MAP_SIZE; i++) {
		if (isspace(input[i])) continue;
		world_map[y][x] = input[i];
		x++;
		if (x >= MAP_SIZE) {
			x = 0;
			y++;
		}
	}
	sx_xunlock(&hyrule_sx);
	return (0);
}

/* Write /dev/hyrule/characters/link/location/move */
static int
hyrule_move_write(struct cdev *dev, struct uio *uio, int ioflag)
{
	char cmd[16];
	int error, len;
	int nx = link_x, ny = link_y;

	sx_xlock(&hyrule_sx);
	len = MIN(uio->uio_resid, sizeof(cmd) - 1);
	error = uiomove(cmd, len, uio);
	if (error) {
		sx_xunlock(&hyrule_sx);
		return (error);
	}
	cmd[len] = '\0';

	if (strncmp(cmd, "up", 2) == 0 || cmd[0] == 'n') ny--;
	else if (strncmp(cmd, "down", 4) == 0 || cmd[0] == 's') ny++;
	else if (strncmp(cmd, "left", 4) == 0 || cmd[0] == 'w') nx--;
	else if (strncmp(cmd, "right", 5) == 0 || cmd[0] == 'e') nx++;
	else {
		sx_xunlock(&hyrule_sx);
		return (EINVAL);
	}

	if (is_accessible(nx, ny)) {
		link_x = nx;
		link_y = ny;
		printf("[HYRULE] Link moved to (%d, %d)\n", link_x, link_y);
	} else {
		printf("[HYRULE] Path blocked at (%d, %d)!\n", nx, ny);
	}

	sx_xunlock(&hyrule_sx);
	return (0);
}

struct cdevsw hyrule_map_cdevsw = {
	.d_version = D_VERSION,
	.d_open = hyrule_open,
	.d_close = hyrule_close,
	.d_read = hyrule_map_read,
	.d_ioctl = hyrule_ioctl,
	.d_poll = hyrule_poll,
	.d_mmap = hyrule_mmap,
	.d_strategy = hyrule_strategy,
	.d_kqfilter = hyrule_kqfilter,
	.d_fdopen = hyrule_fdopen,
	.d_mmap_single = hyrule_mmap_single,
	.d_purge = hyrule_purge,
	.d_name = "hyrule_map",
};

struct cdevsw hyrule_map_config_cdevsw = {
	.d_version = D_VERSION,
	.d_open = hyrule_open,
	.d_close = hyrule_close,
	.d_read = hyrule_map_config_read,
	.d_write = hyrule_map_config_write,
	.d_ioctl = hyrule_ioctl,
	.d_poll = hyrule_poll,
	.d_mmap = hyrule_mmap,
	.d_strategy = hyrule_strategy,
	.d_kqfilter = hyrule_kqfilter,
	.d_fdopen = hyrule_fdopen,
	.d_mmap_single = hyrule_mmap_single,
	.d_purge = hyrule_purge,
	.d_name = "hyrule_config",
};

struct cdevsw hyrule_move_cdevsw = {
	.d_version = D_VERSION,
	.d_open = hyrule_open,
	.d_close = hyrule_close,
	.d_write = hyrule_move_write,
	.d_ioctl = hyrule_ioctl,
	.d_poll = hyrule_poll,
	.d_mmap = hyrule_mmap,
	.d_strategy = hyrule_strategy,
	.d_kqfilter = hyrule_kqfilter,
	.d_fdopen = hyrule_fdopen,
	.d_mmap_single = hyrule_mmap_single,
	.d_purge = hyrule_purge,
	.d_name = "hyrule_move",
};

void
hyrule_map_init(void)
{
	int x, y;
	/* Initialize map with some fields and a dungeon */
	for (y = 0; y < MAP_SIZE; y++) {
		for (x = 0; x < MAP_SIZE; x++) {
			world_map[y][x] = 'f';
		}
	}
	world_map[5][5] = 'd'; /* Dungeon in center (5,5) */
	world_map[5][0] = 'W'; /* Blocked woods at (0,5) */
	link_x = 0;
	link_y = 0;
}
