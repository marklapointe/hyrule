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
#include <sys/malloc.h>

/*
 * Hyrule Input Handling
 */

/* 
 * Variables
 */
static char buttonAMapping[128] = "";
static char buttonBMapping[128] = "";

/* Controller nodes */
static struct hyruleProperty *controllerProps[8] = { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL };

static const char *controllerNames[] = { 
	"console/controller/up", 
	"console/controller/down", 
	"console/controller/left", 
	"console/controller/right", 
	"console/controller/a", 
	"console/controller/b",
	"console/controller/select",
	"console/controller/start"
};

static const char *controllerMsgs[] = { 
	"Moved up\n", 
	"Moved down\n", 
	"Moved left\n", 
	"Moved right\n", 
	"A button pressed\n", 
	"B button pressed\n",
	"Select button pressed\n",
	"Start button pressed\n"
};

static int controllerDx[] = { 0, 0, -1, 1 };
static int controllerDy[] = { -1, 1, 0, 0 };

/* Task for asynchronous update */
static struct task controllerUpdateTask;

static d_read_t hyruleControllerRead;
static d_write_t hyruleControllerWrite;

struct cdevsw hyruleControllerCharacterDeviceSwitch = {
	.d_version = D_VERSION,
	.d_open = hyruleOpen,
	.d_close = hyruleClose,
	.d_read = hyruleControllerRead,
	.d_write = hyruleControllerWrite,
	.d_name = "hyrule_controller",
};

/* State for the secret "cheat code" sequence */
static int comboSequenceIndex = 0;
static const int comboSequence[] = { 0, 0, 1, 1, 2, 3, 2, 3, 5, 4, 7 };

/*
 * Methods (Prototypes)
 */
static void hyruleUpdateControllerNodesTask(void *context, int pending);
static void checkComboSeq(int inputButtonIndex);

/*
 * Methods (Definitions)
 */

static void
checkComboSeq(int inputButtonIndex)
{
	/* 
	 * Check if the pressed button matches the next expected button 
	 * in the secret combo sequence.
	 */
	if (inputButtonIndex == comboSequence[comboSequenceIndex]) {
		comboSequenceIndex++;
		/* If the full sequence is completed... */
		if (comboSequenceIndex == (sizeof(comboSequence) / sizeof(comboSequence[0]))) {
			printf("[HYRULE] Link feels a strange surge of power!\n");
			/* Activate cheat mode. */
			hyruleInvincible = 1;
			hyruleUpdateStatusNodes();
			comboSequenceIndex = 0;
		}
	} else {
		/* If the sequence is broken, reset or restart from the first button. */
		if (inputButtonIndex == comboSequence[0])
			comboSequenceIndex = 1;
		else
			comboSequenceIndex = 0;
	}
}

static void
hyruleUpdateControllerNodesTask(void *context, int pending)
{
	int playerX, playerY, i;
	struct hyruleProperty *nodesToRemove[4] = { NULL, NULL, NULL, NULL };

	/* 
	 * Acquire a shared exclusion lock to update the available controller 
	 * button nodes based on the player's current location and map accessibility.
	 */
	sx_xlock(&hyruleSharedExclusion);
	
	playerX = hyruleGetPropInt("characters/link/location/x", 0);
	playerY = hyruleGetPropInt("characters/link/location/y", 0);

	/* Check directional buttons (up, down, left, right). */
	for (i = 0; i < 4; i++) {
		int nextX = playerX + controllerDx[i];
		int nextY = playerY + controllerDy[i];

		/* If the next tile is accessible, ensure the corresponding button node exists. */
		if (hyruleMapIsAccessible(nextX, nextY)) {
			if (controllerProps[i] == NULL) {
				addHyrulePropertyNodeCustom(controllerNames[i], "", &hyruleControllerCharacterDeviceSwitch);
				struct hyruleProperty *property;
				mtx_lock(&hyruleMutex);
				LIST_FOREACH(property, &propertyList, next) {
					if (strcmp(property->name, controllerNames[i]) == 0) {
						controllerProps[i] = property;
						break;
					}
				}
				mtx_unlock(&hyruleMutex);
			}
		} else {
			/* If the tile is blocked, mark the button node for removal. */
			if (controllerProps[i] != NULL) {
				nodesToRemove[i] = controllerProps[i];
				controllerProps[i] = NULL;
			}
		}
	}
	sx_xunlock(&hyruleSharedExclusion);

	/* Perform node removal outside of the lock. */
	for (i = 0; i < 4; i++) {
		if (nodesToRemove[i] != NULL)
			removeHyrulePropertyNode(nodesToRemove[i]);
	}
}

void
hyruleUpdateControllerNodes(void)
{
	taskqueue_enqueue(taskqueue_thread, &controllerUpdateTask);
}

static int
hyruleControllerRead(struct cdev *dev, struct uio *userIo, int ioflag)
{
	struct hyruleProperty *property = dev->si_drv1;
	int i, buttonIndex = -1;

	/* Seek is not supported for controller button interaction. */
	if (userIo->uio_offset > 0)
		return (0);

	/* Identify which button was 'read'. */
	for (i = 0; i < 8; i++) {
		if (property == controllerProps[i]) {
			buttonIndex = i;
			break;
		}
	}

	/* If the property doesn't match any controller button, something is wrong. */
	if (buttonIndex == -1)
		return (ENXIO);

	sx_xlock(&hyruleSharedExclusion);
	if (!hyruleIsActive()) {
		sx_xunlock(&hyruleSharedExclusion);
		return (EACCES);
	}

	/* Update the cheat combo state. */
	checkComboSeq(buttonIndex);

	/* Logic for movement buttons (Indices 0-3). */
	if (buttonIndex < 4) {
		int nextX = hyruleGetPropInt("characters/link/location/x", 0) + controllerDx[buttonIndex];
		int nextY = hyruleGetPropInt("characters/link/location/y", 0) + controllerDy[buttonIndex];

		hyruleSetPropInt("characters/link/location/x", nextX);
		hyruleSetPropInt("characters/link/location/y", nextY);
		printf("[HYRULE] Link moved to (%d, %d) via %s\n", nextX, nextY, controllerNames[buttonIndex]);
	} else {
		/* Logic for action buttons (A, B, Select, Start). */
		char *buttonMapping = (buttonIndex == 4) ? buttonAMapping : buttonBMapping;
		if (buttonMapping[0] != '\0') {
			/* If the button is mapped to an item, return that item's current value. */
			struct hyruleProperty *itemProperty = NULL;
			mtx_lock(&hyruleMutex);
			LIST_FOREACH(itemProperty, &propertyList, next) {
				if (strcmp(itemProperty->name, buttonMapping) == 0) {
					break;
				}
			}
			mtx_unlock(&hyruleMutex);
			if (itemProperty) {
				int error = uiomove(itemProperty->value, strlen(itemProperty->value), userIo);
				sx_xunlock(&hyruleSharedExclusion);
				return (error);
			}
		}
	}
	
	sx_xunlock(&hyruleSharedExclusion);

	/* Return the default message for the button press. */
	uiomove(__DECONST(char *, controllerMsgs[buttonIndex]), strlen(controllerMsgs[buttonIndex]), userIo);
	
	/* If Link moved, we need to refresh the available nodes. */
	if (buttonIndex < 4) {
		hyruleUpdateControllerNodes();
		hyruleUpdateLocalNodes();
	}

	return (0);
}

static int
hyruleControllerWrite(struct cdev *dev, struct uio *userIo, int ioflag)
{
	struct hyruleProperty *property = dev->si_drv1;
	int i, buttonIndex = -1;

	/* Identify which button is being written to. */
	for (i = 0; i < 8; i++) {
		if (property == controllerProps[i]) {
			buttonIndex = i;
			break;
		}
	}

	if (buttonIndex == -1)
		return (ENXIO);

	sx_xlock(&hyruleSharedExclusion);
	if (!hyruleIsActive()) {
		sx_xunlock(&hyruleSharedExclusion);
		return (EACCES);
	}

	/* Check for cheat sequence. */
	checkComboSeq(buttonIndex);

	if (buttonIndex < 4) {
		/* Movement buttons: update coordinates. */
		int nextX = hyruleGetPropInt("characters/link/location/x", 0) + controllerDx[buttonIndex];
		int nextY = hyruleGetPropInt("characters/link/location/y", 0) + controllerDy[buttonIndex];

		hyruleSetPropInt("characters/link/location/x", nextX);
		hyruleSetPropInt("characters/link/location/y", nextY);
		printf("[HYRULE] Link moved to (%d, %d) via write to %s\n", nextX, nextY, controllerNames[buttonIndex]);
		userIo->uio_resid = 0;
	} else {
		/* Action buttons: allow mapping to item nodes. */
		char inputBuffer[128];
		int error;
		int inputLength = MIN(userIo->uio_resid, sizeof(inputBuffer) - 1);
		error = uiomove(inputBuffer, inputLength, userIo);
		if (error) {
			sx_xunlock(&hyruleSharedExclusion);
			return (error);
		}
		inputBuffer[inputLength] = '\0';
		/* Strip trailing newlines. */
		while (inputLength > 0 && (inputBuffer[inputLength-1] == '\n' || inputBuffer[inputLength-1] == '\r')) {
			inputBuffer[--inputLength] = '\0';
		}

		/* If input matches an item path, perform the mapping. */
		if (strncmp(inputBuffer, "characters/link/items/", 22) == 0) {
			struct hyruleProperty *itemProperty = NULL;
			mtx_lock(&hyruleMutex);
			LIST_FOREACH(itemProperty, &propertyList, next) {
				if (strcmp(itemProperty->name, inputBuffer) == 0) {
					break;
				}
			}
			mtx_unlock(&hyruleMutex);
			if (itemProperty) {
				char *buttonMapping = (buttonIndex == 4) ? buttonAMapping : buttonBMapping;
				strlcpy(buttonMapping, inputBuffer, 128);
				printf("[HYRULE] Button %c mapped to %s\n", (buttonIndex == 4) ? 'A' : 'B', inputBuffer);
			}
		}
	}
	
	sx_xunlock(&hyruleSharedExclusion);

	/* Trigger node updates if player moved. */
	if (buttonIndex < 4) {
		hyruleUpdateControllerNodes();
		hyruleUpdateLocalNodes();
	}

	return (0);
}

void
hyruleInputDrain(void)
{
	taskqueue_drain(taskqueue_thread, &controllerUpdateTask);
}

void
hyruleInputInit(void)
{
	/* Initialize the background task for controller node updates. */
	TASK_INIT(&controllerUpdateTask, 0, hyruleUpdateControllerNodesTask, NULL);

	/* Pre-create the main action buttons. */
	addHyrulePropertyNodeCustom("console/controller/a", "", &hyruleControllerCharacterDeviceSwitch);
	addHyrulePropertyNodeCustom("console/controller/b", "", &hyruleControllerCharacterDeviceSwitch);
	addHyrulePropertyNodeCustom("console/controller/select", "", &hyruleControllerCharacterDeviceSwitch);
	addHyrulePropertyNodeCustom("console/controller/start", "", &hyruleControllerCharacterDeviceSwitch);

	/* Cache the action button property pointers for fast access. */
	struct hyruleProperty *property;
	mtx_lock(&hyruleMutex);
	LIST_FOREACH(property, &propertyList, next) {
		if (strcmp(property->name, "console/controller/a") == 0)
			controllerProps[4] = property;
		else if (strcmp(property->name, "console/controller/b") == 0)
			controllerProps[5] = property;
		else if (strcmp(property->name, "console/controller/select") == 0)
			controllerProps[6] = property;
		else if (strcmp(property->name, "console/controller/start") == 0)
			controllerProps[7] = property;
	}
	mtx_unlock(&hyruleMutex);
}
