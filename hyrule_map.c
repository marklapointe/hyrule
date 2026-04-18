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
#include <sys/taskqueue.h>

/*
 * Hyrule World Map and Location Management
 */

/* 
 * Variables
 */
static char worldMap[MAP_SIZE][MAP_SIZE];
static char worldEntrances[MAP_SIZE][MAP_SIZE];

/* Local nodes under /dev/hyrule/map/local/ */
static struct hyruleProp *localEntranceNode = NULL;
static struct hyruleProp *localExitNode = NULL;
static struct hyruleProp *localNextNode = NULL;
static struct hyruleProp *localPrevNode = NULL;
static struct hyruleProp *localTreasureNode = NULL;
static struct hyruleProp *localBossNode = NULL;
static struct hyruleProp *localRoomNode = NULL;

/* State tracking for internal locations */
static int insideEntrance = 0;
static char currentEntranceType[32] = "";

/* Task for asynchronous update */
static struct task localUpdateTask;

/* Device switches */
static d_read_t hyruleLocalRead;
static d_write_t hyruleLocalWrite;
static d_close_t hyruleLocalClose;

struct cdevsw hyruleLocalCdevsw = {
	.d_version = D_VERSION,
	.d_open = hyruleOpen,
	.d_close = hyruleLocalClose,
	.d_read = hyruleLocalRead,
	.d_write = hyruleLocalWrite,
	.d_name = "hyrule_local",
};

static d_read_t hyruleMapRead;
struct cdevsw hyruleMapCdevsw = {
	.d_version = D_VERSION,
	.d_open = hyruleOpen,
	.d_close = hyruleClose,
	.d_read = hyruleMapRead,
	.d_ioctl = hyruleIoctl,
	.d_poll = hyrulePoll,
	.d_mmap = hyruleMmap,
	.d_strategy = hyruleStrategy,
	.d_kqfilter = hyruleKqfilter,
	.d_fdopen = hyruleFdopen,
	.d_mmap_single = hyruleMmapSingle,
	.d_purge = hyrulePurge,
	.d_name = "hyrule_map",
};

static d_read_t hyruleMapConfigRead;
static d_write_t hyruleMapConfigWrite;
struct cdevsw hyruleMapConfigCdevsw = {
	.d_version = D_VERSION,
	.d_open = hyruleOpen,
	.d_close = hyruleClose,
	.d_read = hyruleMapConfigRead,
	.d_write = hyruleMapConfigWrite,
	.d_ioctl = hyruleIoctl,
	.d_poll = hyrulePoll,
	.d_mmap = hyruleMmap,
	.d_strategy = hyruleStrategy,
	.d_kqfilter = hyruleKqfilter,
	.d_fdopen = hyruleFdopen,
	.d_mmap_single = hyruleMmapSingle,
	.d_purge = hyrulePurge,
	.d_name = "hyrule_config",
};

/*
 * Methods (Prototypes)
 */
static void hyruleUpdateLocalNodesTask(void *context, int pending);
static char getMapSymbol(char type);

/*
 * Methods (Definitions)
 */

static void
hyruleUpdateLocalNodesTask(void *context, int pending)
{
	struct hyruleProp *toRemove[16] = { NULL };
	int trIdx = 0;

	sx_xlock(&hyruleSx);
	int x = hyruleGetPropInt("characters/link/location/x", 0);
	int y = hyruleGetPropInt("characters/link/location/y", 0);
	char entrance = 0;

	if (x >= 0 && x < MAP_SIZE && y >= 0 && y < MAP_SIZE)
		entrance = worldEntrances[y][x];

	if (insideEntrance) {
		if (localEntranceNode != NULL) {
			toRemove[trIdx++] = localEntranceNode;
			localEntranceNode = NULL;
		}

		int dungeon = hyruleGetPropInt("characters/link/location/dungeon", 0);
		int room = hyruleGetPropInt("characters/link/location/room", 0);

		if (room == 0) {
			if (localExitNode == NULL) {
				addHyruleNodeCustom("map/local/exit", "", &hyruleLocalCdevsw);
				struct hyruleProp *p;
				mtx_lock(&hyruleMtx);
				LIST_FOREACH(p, &propList, next) {
					if (strcmp(p->name, "map/local/exit") == 0) {
						localExitNode = p;
						break;
					}
				}
				mtx_unlock(&hyruleMtx);
			}
		} else {
			if (localExitNode != NULL) {
				toRemove[trIdx++] = localExitNode;
				localExitNode = NULL;
			}
		}

		if (dungeon > 0) {
			if (localRoomNode == NULL) {
				addHyruleNodeCustom("map/local/room", "", &hyruleLocalCdevsw);
				struct hyruleProp *p;
				mtx_lock(&hyruleMtx);
				LIST_FOREACH(p, &propList, next) {
					if (strcmp(p->name, "map/local/room") == 0) {
						localRoomNode = p;
						break;
					}
				}
				mtx_unlock(&hyruleMtx);
			}

			if (room < 9) {
				if (localNextNode == NULL) {
					addHyruleNodeCustom("map/local/next", "", &hyruleLocalCdevsw);
					struct hyruleProp *p;
					mtx_lock(&hyruleMtx);
					LIST_FOREACH(p, &propList, next) {
						if (strcmp(p->name, "map/local/next") == 0) {
							localNextNode = p;
							break;
						}
					}
					mtx_unlock(&hyruleMtx);
				}
			} else {
				if (localNextNode != NULL) {
					toRemove[trIdx++] = localNextNode;
					localNextNode = NULL;
				}
			}

			if (room > 0) {
				if (localPrevNode == NULL) {
					addHyruleNodeCustom("map/local/prev", "", &hyruleLocalCdevsw);
					struct hyruleProp *p;
					mtx_lock(&hyruleMtx);
					LIST_FOREACH(p, &propList, next) {
						if (strcmp(p->name, "map/local/prev") == 0) {
							localPrevNode = p;
							break;
						}
					}
					mtx_unlock(&hyruleMtx);
				}
			} else {
				if (localPrevNode != NULL) {
					toRemove[trIdx++] = localPrevNode;
					localPrevNode = NULL;
				}
			}
		}
	} else {
		if (localExitNode != NULL) toRemove[trIdx++] = localExitNode; localExitNode = NULL;
		if (localNextNode != NULL) toRemove[trIdx++] = localNextNode; localNextNode = NULL;
		if (localPrevNode != NULL) toRemove[trIdx++] = localPrevNode; localPrevNode = NULL;
		if (localTreasureNode != NULL) toRemove[trIdx++] = localTreasureNode; localTreasureNode = NULL;
		if (localBossNode != NULL) toRemove[trIdx++] = localBossNode; localBossNode = NULL;
		if (localRoomNode != NULL) toRemove[trIdx++] = localRoomNode; localRoomNode = NULL;

		if (entrance != 0) {
			char path[64];
			const char *ename;
			switch (entrance) {
				case 'c': ename = "cave"; break;
				case 's': ename = "shop"; break;
				case '1': ename = "dungeon1"; break;
				case '2': ename = "dungeon2"; break;
				case '3': ename = "dungeon3"; break;
				case 'g': ename = "ganon"; break;
				case 'u': ename = "upgrade"; break;
				default: ename = "entrance"; break;
			}
			snprintf(path, sizeof(path), "map/local/%s", ename);

			if (localEntranceNode == NULL || strcmp(localEntranceNode->name, path) != 0) {
				if (localEntranceNode != NULL) {
					toRemove[trIdx++] = localEntranceNode;
					localEntranceNode = NULL;
				}
				addHyruleNodeCustom(path, "", &hyruleLocalCdevsw);
				struct hyruleProp *p;
				mtx_lock(&hyruleMtx);
				LIST_FOREACH(p, &propList, next) {
					if (strcmp(p->name, path) == 0) {
						localEntranceNode = p;
						break;
					}
				}
				mtx_unlock(&hyruleMtx);
				strlcpy(currentEntranceType, ename, sizeof(currentEntranceType));
			}
		} else {
			if (localEntranceNode != NULL) {
				toRemove[trIdx++] = localEntranceNode;
				localEntranceNode = NULL;
			}
		}
	}
	sx_xunlock(&hyruleSx);

	for (int i = 0; i < trIdx; i++) {
		if (toRemove[i] != NULL)
			removeHyruleNode(toRemove[i]);
	}
}

void
hyruleUpdateLocalNodes(void)
{
	taskqueue_enqueue(taskqueue_thread, &localUpdateTask);
}

static int
hyruleLocalRead(struct cdev *dev, struct uio *uio, int ioflag)
{
	struct hyruleProp *p = dev->si_drv1;
	char msg[512] = "";
	int msgLen;

	if (uio->uio_offset > 0)
		return (0);

	sx_xlock(&hyruleSx);
	if (!hyruleIsActive()) {
		sx_xunlock(&hyruleSx);
		return (EACCES);
	}

	if (p == localEntranceNode) {
		if (strcmp(currentEntranceType, "cave") == 0) {
			int hasSword = 0;
			struct hyruleProp *item;
			
			snprintf(msg, sizeof(msg), "Old Knight: IT'S DANGEROUS TO GO ALONE! TAKE THIS. You received the WOODEN SWORD!\n");
			
			mtx_lock(&hyruleMtx);
			LIST_FOREACH(item, &propList, next) {
				if (strcmp(item->name, "characters/link/items/sword") == 0) {
					if (strcmp(item->value, "None\n") != 0) {
						hasSword = 1;
					} else {
						strlcpy(item->value, "Wooden Sword\n", sizeof(item->value));
						hasSword = 1;
					}
					break;
				}
			}
			mtx_unlock(&hyruleMtx);

			if (!hasSword) {
				addHyruleNode("characters/link/items/sword", "Wooden Sword\n");
			}
			hyruleSetPropInt("characters/link/stats/sword_level", 0);
			insideEntrance = 1;
		} else {
			snprintf(msg, sizeof(msg), "Entering %s...\n", currentEntranceType);
			insideEntrance = 1;
		}
	} else if (p == localExitNode) {
		insideEntrance = 0;
		hyruleSetPropInt("characters/link/location/dungeon", 0);
		hyruleSetPropInt("characters/link/location/room", 0);
		snprintf(msg, sizeof(msg), "Exiting to the world map...\n");
	} else if (p == localRoomNode) {
		int dungeon = hyruleGetPropInt("characters/link/location/dungeon", 0);
		int room = hyruleGetPropInt("characters/link/location/room", 0);
		snprintf(msg, sizeof(msg), "Dungeon %d, Room %d\n", dungeon, room);
	} else if (p == localNextNode) {
		int room = hyruleGetPropInt("characters/link/location/room", 0);
		if (room < 9) {
			room++;
			hyruleSetPropInt("characters/link/location/room", room);
			snprintf(msg, sizeof(msg), "Moving forward to room %d...\n", room);
		}
	} else if (p == localPrevNode) {
		int room = hyruleGetPropInt("characters/link/location/room", 0);
		if (room > 0) {
			room--;
			hyruleSetPropInt("characters/link/location/room", room);
			snprintf(msg, sizeof(msg), "Moving back to room %d...\n", room);
		}
	}

	sx_xunlock(&hyruleSx);
	
	msgLen = strlen(msg);
	uiomove(msg, msgLen, uio);
	
	return (0);
}

static int
hyruleLocalWrite(struct cdev *dev, struct uio *uio, int ioflag)
{
	struct hyruleProp *p = dev->si_drv1;

	sx_xlock(&hyruleSx);
	if (!hyruleIsActive()) {
		sx_xunlock(&hyruleSx);
		return (EACCES);
	}

	if (p == localEntranceNode) {
		insideEntrance = 1;
	} else if (p == localExitNode) {
		insideEntrance = 0;
	}

	sx_xunlock(&hyruleSx);
	uio->uio_resid = 0;
	return (0);
}

static int
hyruleLocalClose(struct cdev *dev, int fflag, int devtype, struct thread *td)
{
	hyruleUpdateLocalNodes();
	hyruleUpdateControllerNodes();
	return (0);
}

static char
getMapSymbol(char type)
{
	switch (type) {
	case 'f': return ('.');
	case 'W': return ('W');
	case 'e': return (' ');
	case 'a': return ('F');
	case 'c': return ('C');
	case 's': return ('S');
	case '1': return ('1');
	case '2': return ('2');
	case '3': return ('3');
	case 'g': return ('G');
	case 'u': return ('U');
	default:  return (type);
	}
}

int
hyruleMapIsAccessible(int x, int y)
{
	if (insideEntrance)
		return (0);
	if (x < 0 || x >= MAP_SIZE || y < 0 || y >= MAP_SIZE)
		return (0);
	char c = worldMap[y][x];
	return (islower(c) || isdigit(c));
}

static int
hyruleMapRead(struct cdev *dev, struct uio *uio, int ioflag)
{
	char buf[1536];
	int pos = 0;
	int x, y;

	sx_slock(&hyruleSx);

	if (!hyruleIsActive()) {
		const char *msg = (hyrulePower == 0) ? "POWER OFF\n" : "GAME OVER\n";
		int msgLen = strlen(msg);
		if (uio->uio_offset >= msgLen) {
			sx_sunlock(&hyruleSx);
			return (0);
		}
		int error = uiomove(__DECONST(char *, msg) + uio->uio_offset, msgLen - uio->uio_offset, uio);
		sx_sunlock(&hyruleSx);
		return (error);
	}

	int lx = hyruleGetPropInt("characters/link/location/x", 0);
	int ly = hyruleGetPropInt("characters/link/location/y", 0);

	pos += snprintf(buf + pos, sizeof(buf) - pos, "--- Hyrule Map ---\n");
	for (y = 0; y < MAP_SIZE; y++) {
		for (x = 0; x < MAP_SIZE; x++) {
			pos += snprintf(buf + pos, sizeof(buf) - pos, "+---");
		}
		pos += snprintf(buf + pos, sizeof(buf) - pos, "+\n");
		for (x = 0; x < MAP_SIZE; x++) {
			if (x == lx && y == ly)
				pos += snprintf(buf + pos, sizeof(buf) - pos, "| L ");
			else
				pos += snprintf(buf + pos, sizeof(buf) - pos, "| %c ", getMapSymbol(worldMap[y][x]));
		}
		pos += snprintf(buf + pos, sizeof(buf) - pos, "|\n");
	}
	for (x = 0; x < MAP_SIZE; x++) {
		pos += snprintf(buf + pos, sizeof(buf) - pos, "+---");
	}
	pos += snprintf(buf + pos, sizeof(buf) - pos, "+\n");

	if (uio->uio_offset >= pos) {
		sx_sunlock(&hyruleSx);
		return (0);
	}

	int error = uiomove(buf + uio->uio_offset, pos - uio->uio_offset, uio);
	sx_sunlock(&hyruleSx);
	return (error);
}

void
hyruleMapGetConfig(char *buf, size_t size)
{
	int x, y, pos = 0;
	for (y = 0; y < MAP_SIZE; y++) {
		for (x = 0; x < MAP_SIZE; x++) {
			if (pos < size - 1)
				buf[pos++] = worldMap[y][x];
		}
		if (pos < size - 1)
			buf[pos++] = '\n';
	}
	buf[pos] = '\0';
}

void
hyruleMapSetConfig(const char *input, size_t len)
{
	int x = 0, y = 0, i;
	for (i = 0; i < len && y < MAP_SIZE; i++) {
		if (isspace(input[i])) continue;
		worldMap[y][x] = input[i];
		x++;
		if (x >= MAP_SIZE) {
			x = 0;
			y++;
		}
	}
	hyruleUpdateControllerNodes();
}

static int
hyruleMapConfigRead(struct cdev *dev, struct uio *uio, int ioflag)
{
	char buf[128];
	int x, y, pos = 0;

	sx_slock(&hyruleSx);
	if (!hyruleIsActive()) {
		const char *msg = (hyrulePower == 0) ? "POWER OFF\n" : "GAME OVER\n";
		int msgLen = strlen(msg);
		if (uio->uio_offset >= msgLen) {
			sx_sunlock(&hyruleSx);
			return (0);
		}
		int error = uiomove(__DECONST(char *, msg) + uio->uio_offset, msgLen - uio->uio_offset, uio);
		sx_sunlock(&hyruleSx);
		return (error);
	}

	for (y = 0; y < MAP_SIZE; y++) {
		for (x = 0; x < MAP_SIZE; x++) {
			buf[pos++] = worldMap[y][x];
		}
		buf[pos++] = '\n';
	}
	buf[pos] = '\0';

	if (uio->uio_offset >= pos) {
		sx_sunlock(&hyruleSx);
		return (0);
	}
	int error = uiomove(buf + uio->uio_offset, pos - uio->uio_offset, uio);
	sx_sunlock(&hyruleSx);
	return (error);
}

static int
hyruleMapConfigWrite(struct cdev *dev, struct uio *uio, int ioflag)
{
	char input[256];
	int error, len, x = 0, y = 0, i;

	sx_xlock(&hyruleSx);
	if (!hyrulePower) {
		sx_xunlock(&hyruleSx);
		return (EACCES);
	}
	len = MIN(uio->uio_resid, sizeof(input) - 1);
	error = uiomove(input, len, uio);
	if (error) {
		sx_xunlock(&hyruleSx);
		return (error);
	}
	input[len] = '\0';
	for (i = 0; i < len && y < MAP_SIZE; i++) {
		if (isspace(input[i])) continue;
		worldMap[y][x] = input[i];
		x++;
		if (x >= MAP_SIZE) {
			x = 0;
			y++;
		}
	}
	sx_xunlock(&hyruleSx);
	hyruleUpdateControllerNodes();
	return (0);
}

void
hyruleMapDrain(void)
{
	taskqueue_drain(taskqueue_thread, &localUpdateTask);
}

void
hyruleMapInit(void)
{
	int x, y;
	TASK_INIT(&localUpdateTask, 0, hyruleUpdateLocalNodesTask, NULL);
	for (y = 0; y < MAP_SIZE; y++) {
		for (x = 0; x < MAP_SIZE; x++) {
			worldMap[y][x] = 'f';
			worldEntrances[y][x] = 0;
		}
	}
	worldMap[2][2] = '1';
	worldMap[2][7] = '2';
	worldMap[7][2] = '3';
	worldMap[5][5] = 'g';
	worldMap[9][9] = 'u';
	worldMap[5][0] = 'W';
	worldEntrances[0][0] = 'c';
	worldEntrances[2][2] = '1';
	worldEntrances[2][7] = '2';
	worldEntrances[7][2] = '3';
	worldEntrances[5][5] = 'g';
	worldEntrances[9][9] = 'u';
	hyruleUpdateLocalNodes();
}
