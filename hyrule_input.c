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
static struct hyruleProp *controllerProps[8] = { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL };

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

struct cdevsw hyruleControllerCdevsw = {
	.d_version = D_VERSION,
	.d_open = hyruleOpen,
	.d_close = hyruleClose,
	.d_read = hyruleControllerRead,
	.d_write = hyruleControllerWrite,
	.d_name = "hyrule_controller",
};

/* State for the secret "cheat code" sequence */
static int hIdx = 0;
static const int hSeq[] = { 0, 0, 1, 1, 2, 3, 2, 3, 5, 4, 7 };

/*
 * Methods (Prototypes)
 */
static void hyruleUpdateControllerNodesTask(void *context, int pending);
static void checkComboSeq(int input);

/*
 * Methods (Definitions)
 */

static void
checkComboSeq(int input)
{
	if (input == hSeq[hIdx]) {
		hIdx++;
		if (hIdx == (sizeof(hSeq) / sizeof(hSeq[0]))) {
			printf("[HYRULE] Link feels a strange surge of power!\n");
			hyruleInvincible = 1;
			hyruleUpdateStatusNodes();
			hIdx = 0;
		}
	} else {
		if (input == hSeq[0])
			hIdx = 1;
		else
			hIdx = 0;
	}
}

static void
hyruleUpdateControllerNodesTask(void *context, int pending)
{
	int x, y, i;
	struct hyruleProp *toRemove[4] = { NULL, NULL, NULL, NULL };

	sx_xlock(&hyruleSx);
	
	x = hyruleGetPropInt("characters/link/location/x", 0);
	y = hyruleGetPropInt("characters/link/location/y", 0);

	for (i = 0; i < 4; i++) {
		int nx = x + controllerDx[i];
		int ny = y + controllerDy[i];

		if (hyruleMapIsAccessible(nx, ny)) {
			if (controllerProps[i] == NULL) {
				addHyruleNodeCustom(controllerNames[i], "", &hyruleControllerCdevsw);
				struct hyruleProp *p;
				mtx_lock(&hyruleMtx);
				LIST_FOREACH(p, &propList, next) {
					if (strcmp(p->name, controllerNames[i]) == 0) {
						controllerProps[i] = p;
						break;
					}
				}
				mtx_unlock(&hyruleMtx);
			}
		} else {
			if (controllerProps[i] != NULL) {
				toRemove[i] = controllerProps[i];
				controllerProps[i] = NULL;
			}
		}
	}
	sx_xunlock(&hyruleSx);

	for (i = 0; i < 4; i++) {
		if (toRemove[i] != NULL)
			removeHyruleNode(toRemove[i]);
	}
}

void
hyruleUpdateControllerNodes(void)
{
	taskqueue_enqueue(taskqueue_thread, &controllerUpdateTask);
}

static int
hyruleControllerRead(struct cdev *dev, struct uio *uio, int ioflag)
{
	struct hyruleProp *p = dev->si_drv1;
	int i, dir = -1;

	if (uio->uio_offset > 0)
		return (0);

	for (i = 0; i < 8; i++) {
		if (p == controllerProps[i]) {
			dir = i;
			break;
		}
	}

	if (dir == -1)
		return (ENXIO);

	sx_xlock(&hyruleSx);
	if (!hyruleIsActive()) {
		sx_xunlock(&hyruleSx);
		return (EACCES);
	}

	checkComboSeq(dir);

	if (dir < 4) {
		int nx = hyruleGetPropInt("characters/link/location/x", 0) + controllerDx[dir];
		int ny = hyruleGetPropInt("characters/link/location/y", 0) + controllerDy[dir];

		hyruleSetPropInt("characters/link/location/x", nx);
		hyruleSetPropInt("characters/link/location/y", ny);
		printf("[HYRULE] Link moved to (%d, %d) via %s\n", nx, ny, controllerNames[dir]);
	} else {
		char *mapping = (dir == 4) ? buttonAMapping : buttonBMapping;
		if (mapping[0] != '\0') {
			struct hyruleProp *prop = NULL;
			mtx_lock(&hyruleMtx);
			LIST_FOREACH(prop, &propList, next) {
				if (strcmp(prop->name, mapping) == 0) {
					break;
				}
			}
			mtx_unlock(&hyruleMtx);
			if (prop) {
				int error = uiomove(prop->value, strlen(prop->value), uio);
				sx_xunlock(&hyruleSx);
				return (error);
			}
		}
	}
	
	sx_xunlock(&hyruleSx);

	uiomove(__DECONST(char *, controllerMsgs[dir]), strlen(controllerMsgs[dir]), uio);
	
	if (dir < 4) {
		hyruleUpdateControllerNodes();
		hyruleUpdateLocalNodes();
	}

	return (0);
}

static int
hyruleControllerWrite(struct cdev *dev, struct uio *uio, int ioflag)
{
	struct hyruleProp *p = dev->si_drv1;
	int i, dir = -1;

	for (i = 0; i < 8; i++) {
		if (p == controllerProps[i]) {
			dir = i;
			break;
		}
	}

	if (dir == -1)
		return (ENXIO);

	sx_xlock(&hyruleSx);
	if (!hyruleIsActive()) {
		sx_xunlock(&hyruleSx);
		return (EACCES);
	}

	checkComboSeq(dir);

	if (dir < 4) {
		int nx = hyruleGetPropInt("characters/link/location/x", 0) + controllerDx[dir];
		int ny = hyruleGetPropInt("characters/link/location/y", 0) + controllerDy[dir];

		hyruleSetPropInt("characters/link/location/x", nx);
		hyruleSetPropInt("characters/link/location/y", ny);
		printf("[HYRULE] Link moved to (%d, %d) via write to %s\n", nx, ny, controllerNames[dir]);
		uio->uio_resid = 0;
	} else {
		char input[128];
		int error;
		int len = MIN(uio->uio_resid, sizeof(input) - 1);
		error = uiomove(input, len, uio);
		if (error) {
			sx_xunlock(&hyruleSx);
			return (error);
		}
		input[len] = '\0';
		while (len > 0 && (input[len-1] == '\n' || input[len-1] == '\r')) {
			input[--len] = '\0';
		}

		if (strncmp(input, "characters/link/items/", 22) == 0) {
			struct hyruleProp *prop = NULL;
			mtx_lock(&hyruleMtx);
			LIST_FOREACH(prop, &propList, next) {
				if (strcmp(prop->name, input) == 0) {
					break;
				}
			}
			mtx_unlock(&hyruleMtx);
			if (prop) {
				char *mapping = (dir == 4) ? buttonAMapping : buttonBMapping;
				strlcpy(mapping, input, 128);
				printf("[HYRULE] Button %c mapped to %s\n", (dir == 4) ? 'A' : 'B', input);
			}
		}
	}
	
	sx_xunlock(&hyruleSx);

	if (dir < 4) {
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
	TASK_INIT(&controllerUpdateTask, 0, hyruleUpdateControllerNodesTask, NULL);

	addHyruleNodeCustom("console/controller/a", "", &hyruleControllerCdevsw);
	addHyruleNodeCustom("console/controller/b", "", &hyruleControllerCdevsw);
	addHyruleNodeCustom("console/controller/select", "", &hyruleControllerCdevsw);
	addHyruleNodeCustom("console/controller/start", "", &hyruleControllerCdevsw);

	struct hyruleProp *p;
	mtx_lock(&hyruleMtx);
	LIST_FOREACH(p, &propList, next) {
		if (strcmp(p->name, "console/controller/a") == 0)
			controllerProps[4] = p;
		else if (strcmp(p->name, "console/controller/b") == 0)
			controllerProps[5] = p;
		else if (strcmp(p->name, "console/controller/select") == 0)
			controllerProps[6] = p;
		else if (strcmp(p->name, "console/controller/start") == 0)
			controllerProps[7] = p;
	}
	mtx_unlock(&hyruleMtx);
}
