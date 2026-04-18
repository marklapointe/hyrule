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
static struct hyruleProperty *localEntranceNode = NULL;
static struct hyruleProperty *localExitNode = NULL;
static struct hyruleProperty *localNextNode = NULL;
static struct hyruleProperty *localPrevNode = NULL;
static struct hyruleProperty *localTreasureNode = NULL;
static struct hyruleProperty *localBossNode = NULL;
static struct hyruleProperty *localRoomNode = NULL;

/* State tracking for internal locations */
static int insideEntrance = 0;
static char currentEntranceType[32] = "";

/* Task for asynchronous update */
static struct task localUpdateTask;

/* Device switches */
static d_read_t hyruleLocalRead;
static d_write_t hyruleLocalWrite;
static d_close_t hyruleLocalClose;

struct cdevsw hyruleLocalCharacterDeviceSwitch = {
	.d_version = D_VERSION,
	.d_open = hyruleOpen,
	.d_close = hyruleLocalClose,
	.d_read = hyruleLocalRead,
	.d_write = hyruleLocalWrite,
	.d_name = "hyrule_local",
};

static d_read_t hyruleMapRead;
struct cdevsw hyruleMapCharacterDeviceSwitch = {
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
struct cdevsw hyruleMapConfigCharacterDeviceSwitch = {
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
	struct hyruleProperty *nodesToRemove[16] = { NULL };
	int removalIndex = 0;

	/* 
	 * Acquire an exclusive lock because we may be adding or removing 
	 * property nodes based on the current player location.
	 */
	sx_xlock(&hyruleSharedExclusion);
	int playerX = hyruleGetPropInt("characters/link/location/x", 0);
	int playerY = hyruleGetPropInt("characters/link/location/y", 0);
	char entranceType = 0;

	/* Determine if the player is currently standing on an entrance. */
	if (playerX >= 0 && playerX < MAP_SIZE && playerY >= 0 && playerY < MAP_SIZE)
		entranceType = worldEntrances[playerY][playerX];

	/* If the player has entered a building or dungeon... */
	if (insideEntrance) {
		/* Remove the entrance node as we are now 'inside'. */
		if (localEntranceNode != NULL) {
			nodesToRemove[removalIndex++] = localEntranceNode;
			localEntranceNode = NULL;
		}

		int dungeonId = hyruleGetPropInt("characters/link/location/dungeon", 0);
		int roomId = hyruleGetPropInt("characters/link/location/room", 0);

		/* If in the first room (entrance), provide an 'exit' node. */
		if (roomId == 0) {
			if (localExitNode == NULL) {
				addHyrulePropertyNodeCustom("map/local/exit", "", &hyruleLocalCharacterDeviceSwitch);
				struct hyruleProperty *property;
				mtx_lock(&hyruleMutex);
				LIST_FOREACH(property, &propertyList, next) {
					if (strcmp(property->name, "map/local/exit") == 0) {
						localExitNode = property;
						break;
					}
				}
				mtx_unlock(&hyruleMutex);
			}
		} else {
			/* Otherwise, hide the exit node. */
			if (localExitNode != NULL) {
				nodesToRemove[removalIndex++] = localExitNode;
				localExitNode = NULL;
			}
		}

		/* Dungeon specific navigation nodes. */
		if (dungeonId > 0) {
			/* Provide a node to check current room status. */
			if (localRoomNode == NULL) {
				addHyrulePropertyNodeCustom("map/local/room", "", &hyruleLocalCharacterDeviceSwitch);
				struct hyruleProperty *property;
				mtx_lock(&hyruleMutex);
				LIST_FOREACH(property, &propertyList, next) {
					if (strcmp(property->name, "map/local/room") == 0) {
						localRoomNode = property;
						break;
					}
				}
				mtx_unlock(&hyruleMutex);
			}

			/* Provide 'next' node if not at the end of the dungeon. */
			if (roomId < 9) {
				if (localNextNode == NULL) {
					addHyrulePropertyNodeCustom("map/local/next", "", &hyruleLocalCharacterDeviceSwitch);
					struct hyruleProperty *property;
					mtx_lock(&hyruleMutex);
					LIST_FOREACH(property, &propertyList, next) {
						if (strcmp(property->name, "map/local/next") == 0) {
							localNextNode = property;
							break;
						}
					}
					mtx_unlock(&hyruleMutex);
				}
			} else {
				if (localNextNode != NULL) {
					nodesToRemove[removalIndex++] = localNextNode;
					localNextNode = NULL;
				}
			}

			/* Provide 'prev' node if not at the entrance. */
			if (roomId > 0) {
				if (localPrevNode == NULL) {
					addHyrulePropertyNodeCustom("map/local/prev", "", &hyruleLocalCharacterDeviceSwitch);
					struct hyruleProperty *property;
					mtx_lock(&hyruleMutex);
					LIST_FOREACH(property, &propertyList, next) {
						if (strcmp(property->name, "map/local/prev") == 0) {
							localPrevNode = property;
							break;
						}
					}
					mtx_unlock(&hyruleMutex);
				}
			} else {
				if (localPrevNode != NULL) {
					nodesToRemove[removalIndex++] = localPrevNode;
					localPrevNode = NULL;
				}
			}
		}
	} else {
		/* Player is on the world map. Remove all internal navigation nodes. */
		if (localExitNode != NULL) nodesToRemove[removalIndex++] = localExitNode; localExitNode = NULL;
		if (localNextNode != NULL) nodesToRemove[removalIndex++] = localNextNode; localNextNode = NULL;
		if (localPrevNode != NULL) nodesToRemove[removalIndex++] = localPrevNode; localPrevNode = NULL;
		if (localTreasureNode != NULL) nodesToRemove[removalIndex++] = localTreasureNode; localTreasureNode = NULL;
		if (localBossNode != NULL) nodesToRemove[removalIndex++] = localBossNode; localBossNode = NULL;
		if (localRoomNode != NULL) nodesToRemove[removalIndex++] = localRoomNode; localRoomNode = NULL;

		/* If standing on an entrance, create the entrance device node. */
		if (entranceType != 0) {
			char entrancePath[64];
			const char *entranceName;
			switch (entranceType) {
				case 'c': entranceName = "cave"; break;
				case 's': entranceName = "shop"; break;
				case '1': entranceName = "dungeon1"; break;
				case '2': entranceName = "dungeon2"; break;
				case '3': entranceName = "dungeon3"; break;
				case 'g': entranceName = "ganon"; break;
				case 'u': entranceName = "upgrade"; break;
				default: entranceName = "entrance"; break;
			}
			snprintf(entrancePath, sizeof(entrancePath), "map/local/%s", entranceName);

			/* Update the entrance node if it has changed. */
			if (localEntranceNode == NULL || strcmp(localEntranceNode->name, entrancePath) != 0) {
				if (localEntranceNode != NULL) {
					nodesToRemove[removalIndex++] = localEntranceNode;
					localEntranceNode = NULL;
				}
				addHyrulePropertyNodeCustom(entrancePath, "", &hyruleLocalCharacterDeviceSwitch);
				struct hyruleProperty *property;
				mtx_lock(&hyruleMutex);
				LIST_FOREACH(property, &propertyList, next) {
					if (strcmp(property->name, entrancePath) == 0) {
						localEntranceNode = property;
						break;
					}
				}
				mtx_unlock(&hyruleMutex);
				strlcpy(currentEntranceType, entranceName, sizeof(currentEntranceType));
			}
		} else {
			/* Not on an entrance, clear the node. */
			if (localEntranceNode != NULL) {
				nodesToRemove[removalIndex++] = localEntranceNode;
				localEntranceNode = NULL;
			}
		}
	}
	sx_xunlock(&hyruleSharedExclusion);

	/* Safely remove nodes outside of the lock. */
	for (int i = 0; i < removalIndex; i++) {
		if (nodesToRemove[i] != NULL)
			removeHyrulePropertyNode(nodesToRemove[i]);
	}
}

void
hyruleUpdateLocalNodes(void)
{
	taskqueue_enqueue(taskqueue_thread, &localUpdateTask);
}

static int
hyruleLocalRead(struct cdev *dev, struct uio *userIo, int ioflag)
{
	struct hyruleProperty *property = dev->si_drv1;
	char message[512] = "";
	int messageLength;

	/* Only allow reading from the start of the 'file'. */
	if (userIo->uio_offset > 0)
		return (0);

	sx_xlock(&hyruleSharedExclusion);
	/* Generic check for active system status. */
	if (!hyruleIsActive()) {
		sx_xunlock(&hyruleSharedExclusion);
		return (EACCES);
	}

	/* Logic for interacting with dynamic local nodes. */
	if (property == localEntranceNode) {
		if (strcmp(currentEntranceType, "cave") == 0) {
			int hasSword = 0;
			struct hyruleProperty *itemProperty;
			
			snprintf(message, sizeof(message), "Old Knight: IT'S DANGEROUS TO GO ALONE! TAKE THIS. You received the WOODEN SWORD!\n");
			
			mtx_lock(&hyruleMutex);
			LIST_FOREACH(itemProperty, &propertyList, next) {
				if (strcmp(itemProperty->name, "characters/link/items/sword") == 0) {
					if (strcmp(itemProperty->value, "None\n") != 0) {
						hasSword = 1;
					} else {
						strlcpy(itemProperty->value, "Wooden Sword\n", sizeof(itemProperty->value));
						hasSword = 1;
					}
					break;
				}
			}
			mtx_unlock(&hyruleMutex);

			/* If the sword node doesn't exist for some reason, create it. */
			if (!hasSword) {
				addHyrulePropertyNode("characters/link/items/sword", "Wooden Sword\n");
			}
			hyruleSetPropInt("characters/link/stats/sword_level", 0);
			insideEntrance = 1;
		} else {
			snprintf(message, sizeof(message), "Entering %s...\n", currentEntranceType);
			insideEntrance = 1;
		}
	} else if (property == localExitNode) {
		insideEntrance = 0;
		hyruleSetPropInt("characters/link/location/dungeon", 0);
		hyruleSetPropInt("characters/link/location/room", 0);
		snprintf(message, sizeof(message), "Exiting to the world map...\n");
	} else if (property == localRoomNode) {
		int dungeonId = hyruleGetPropInt("characters/link/location/dungeon", 0);
		int roomId = hyruleGetPropInt("characters/link/location/room", 0);
		snprintf(message, sizeof(message), "Dungeon %d, Room %d\n", dungeonId, roomId);
	} else if (property == localNextNode) {
		int roomId = hyruleGetPropInt("characters/link/location/room", 0);
		if (roomId < 9) {
			roomId++;
			hyruleSetPropInt("characters/link/location/room", roomId);
			snprintf(message, sizeof(message), "Moving forward to room %d...\n", roomId);
		}
	} else if (property == localPrevNode) {
		int roomId = hyruleGetPropInt("characters/link/location/room", 0);
		if (roomId > 0) {
			roomId--;
			hyruleSetPropInt("characters/link/location/room", roomId);
			snprintf(message, sizeof(message), "Moving back to room %d...\n", roomId);
		}
	}

	sx_xunlock(&hyruleSharedExclusion);
	
	/* Copy the resulting interaction message to userspace. */
	messageLength = strlen(message);
	uiomove(message, messageLength, userIo);
	
	return (0);
}

static int
hyruleLocalWrite(struct cdev *dev, struct uio *userIo, int ioflag)
{
	struct hyruleProperty *property = dev->si_drv1;

	/* 
	 * Acquire an exclusive lock to modify internal state.
	 */
	sx_xlock(&hyruleSharedExclusion);
	if (!hyruleIsActive()) {
		sx_xunlock(&hyruleSharedExclusion);
		return (EACCES);
	}

	/* Simple toggle logic for entering/exiting via write. */
	if (property == localEntranceNode) {
		insideEntrance = 1;
	} else if (property == localExitNode) {
		insideEntrance = 0;
	}

	sx_xunlock(&hyruleSharedExclusion);
	/* Consume the user input (discarding data but marking it as 'processed'). */
	userIo->uio_resid = 0;
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
hyruleMapRead(struct cdev *dev, struct uio *userIo, int ioflag)
{
	char displayBuffer[1536];
	int currentPosition = 0;
	int mapX, mapY;

	/* 
	 * Use a shared lock to read the current world map state.
	 */
	sx_slock(&hyruleSharedExclusion);

	/* Check if the map should be 'visible' based on power/health. */
	if (!hyruleIsActive()) {
		const char *message = (hyrulePower == 0) ? "POWER OFF\n" : "GAME OVER\n";
		int messageLength = strlen(message);
		if (userIo->uio_offset >= messageLength) {
			sx_sunlock(&hyruleSharedExclusion);
			return (0);
		}
		int error = uiomove(__DECONST(char *, message) + userIo->uio_offset, messageLength - userIo->uio_offset, userIo);
		sx_sunlock(&hyruleSharedExclusion);
		return (error);
	}

	/* Get player coordinates for 'L' marker. */
	int playerX = hyruleGetPropInt("characters/link/location/x", 0);
	int playerY = hyruleGetPropInt("characters/link/location/y", 0);

	/* Build the ASCII map in our local buffer. */
	currentPosition += snprintf(displayBuffer + currentPosition, sizeof(displayBuffer) - currentPosition, "--- Hyrule Map ---\n");
	for (mapY = 0; mapY < MAP_SIZE; mapY++) {
		for (mapX = 0; mapX < MAP_SIZE; mapX++) {
			currentPosition += snprintf(displayBuffer + currentPosition, sizeof(displayBuffer) - currentPosition, "+---");
		}
		currentPosition += snprintf(displayBuffer + currentPosition, sizeof(displayBuffer) - currentPosition, "+\n");
		for (mapX = 0; mapX < MAP_SIZE; mapX++) {
			if (mapX == playerX && mapY == playerY)
				currentPosition += snprintf(displayBuffer + currentPosition, sizeof(displayBuffer) - currentPosition, "| L ");
			else
				currentPosition += snprintf(displayBuffer + currentPosition, sizeof(displayBuffer) - currentPosition, "| %c ", getMapSymbol(worldMap[mapY][mapX]));
		}
		currentPosition += snprintf(displayBuffer + currentPosition, sizeof(displayBuffer) - currentPosition, "|\n");
	}
	for (mapX = 0; mapX < MAP_SIZE; mapX++) {
		currentPosition += snprintf(displayBuffer + currentPosition, sizeof(displayBuffer) - currentPosition, "+---");
	}
	currentPosition += snprintf(displayBuffer + currentPosition, sizeof(displayBuffer) - currentPosition, "+\n");

	/* Return EOF if user has read past the map. */
	if (userIo->uio_offset >= currentPosition) {
		sx_sunlock(&hyruleSharedExclusion);
		return (0);
	}

	/* Move the ASCII map from kernel to userspace. */
	int error = uiomove(displayBuffer + userIo->uio_offset, currentPosition - userIo->uio_offset, userIo);
	sx_sunlock(&hyruleSharedExclusion);
	return (error);
}

void
hyruleMapGetConfig(char *buffer, size_t size)
{
	int mapX, mapY, currentPosition = 0;
	/* 
	 * Flatten the 2D world map into a single string for configuration Export.
	 */
	for (mapY = 0; mapY < MAP_SIZE; mapY++) {
		for (mapX = 0; mapX < MAP_SIZE; mapX++) {
			if (currentPosition < size - 1)
				buffer[currentPosition++] = worldMap[mapY][mapX];
		}
		if (currentPosition < size - 1)
			buffer[currentPosition++] = '\n';
	}
	buffer[currentPosition] = '\0';
}

void
hyruleMapSetConfig(const char *input, size_t length)
{
	int mapX = 0, mapY = 0, i;
	/* 
	 * Parse the input string and update the 2D world map. 
	 * We ignore whitespace during parsing.
	 */
	for (i = 0; i < length && mapY < MAP_SIZE; i++) {
		if (isspace(input[i])) continue;
		worldMap[mapY][mapX] = input[i];
		mapX++;
		if (mapX >= MAP_SIZE) {
			mapX = 0;
			mapY++;
		}
	}
	/* Refresh any nodes that depend on the map structure. */
	hyruleUpdateControllerNodes();
}

static int
hyruleMapConfigRead(struct cdev *dev, struct uio *userIo, int ioflag)
{
	char configBuffer[128];
	int mapX, mapY, currentPosition = 0;

	/* Shared lock for reading map configuration. */
	sx_slock(&hyruleSharedExclusion);
	if (!hyruleIsActive()) {
		const char *message = (hyrulePower == 0) ? "POWER OFF\n" : "GAME OVER\n";
		int messageLength = strlen(message);
		if (userIo->uio_offset >= messageLength) {
			sx_sunlock(&hyruleSharedExclusion);
			return (0);
		}
		int error = uiomove(__DECONST(char *, message) + userIo->uio_offset, messageLength - userIo->uio_offset, userIo);
		sx_sunlock(&hyruleSharedExclusion);
		return (error);
	}

	/* Build the flat map string. */
	for (mapY = 0; mapY < MAP_SIZE; mapY++) {
		for (mapX = 0; mapX < MAP_SIZE; mapX++) {
			configBuffer[currentPosition++] = worldMap[mapY][mapX];
		}
		configBuffer[currentPosition++] = '\n';
	}
	configBuffer[currentPosition] = '\0';

	if (userIo->uio_offset >= currentPosition) {
		sx_sunlock(&hyruleSharedExclusion);
		return (0);
	}
	/* Copy configuration to userspace. */
	int error = uiomove(configBuffer + userIo->uio_offset, currentPosition - userIo->uio_offset, userIo);
	sx_sunlock(&hyruleSharedExclusion);
	return (error);
}

static int
hyruleMapConfigWrite(struct cdev *dev, struct uio *userIo, int ioflag)
{
	char inputBuffer[256];
	int error, length, mapX = 0, mapY = 0, i;

	/* Exclusive lock for modifying map configuration. */
	sx_xlock(&hyruleSharedExclusion);
	if (!hyrulePower) {
		sx_xunlock(&hyruleSharedExclusion);
		return (EACCES);
	}
	/* Read the new map configuration from userspace. */
	length = MIN(userIo->uio_resid, sizeof(inputBuffer) - 1);
	error = uiomove(inputBuffer, length, userIo);
	if (error) {
		sx_xunlock(&hyruleSharedExclusion);
		return (error);
	}
	inputBuffer[length] = '\0';
	/* Apply the new map configuration. */
	for (i = 0; i < length && mapY < MAP_SIZE; i++) {
		if (isspace(inputBuffer[i])) continue;
		worldMap[mapY][mapX] = inputBuffer[i];
		mapX++;
		if (mapX >= MAP_SIZE) {
			mapX = 0;
			mapY++;
		}
	}
	sx_xunlock(&hyruleSharedExclusion);
	/* Update dependent nodes. */
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
	int mapX, mapY;

	/* Initialize the background task for local node updates. */
	TASK_INIT(&localUpdateTask, 0, hyruleUpdateLocalNodesTask, NULL);

	/* Initialize the map with default values ('f' for field, 0 for no entrance). */
	for (mapY = 0; mapY < MAP_SIZE; mapY++) {
		for (mapX = 0; mapX < MAP_SIZE; mapX++) {
			worldMap[mapY][mapX] = 'f';
			worldEntrances[mapY][mapX] = 0;
		}
	}

	/* Manually set up some iconic locations on the map. */
	worldMap[2][2] = '1'; /* Dungeon 1 */
	worldMap[2][7] = '2'; /* Dungeon 2 */
	worldMap[7][2] = '3'; /* Dungeon 3 */
	worldMap[5][5] = 'g'; /* Ganon's Castle */
	worldMap[9][9] = 'u'; /* Secret Upgrade */
	worldMap[5][0] = 'W'; /* Water/Waterfall */

	/* Define entrance types for the locations above. */
	worldEntrances[0][0] = 'c'; /* Starting Cave */
	worldEntrances[2][2] = '1';
	worldEntrances[2][7] = '2';
	worldEntrances[7][2] = '3';
	worldEntrances[5][5] = 'g';
	worldEntrances[9][9] = 'u';

	/* Trigger initial local node creation. */
	hyruleUpdateLocalNodes();
}
