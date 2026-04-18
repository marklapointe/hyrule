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
 *
 * This file implements the "game controller" logic. It allows the user to
 * interact with the module by reading from or writing to /dev/hyrule/console/controller/ nodes.
 */

/* Mappings for the A and B buttons to specific items */
static char button_a_mapping[128] = "";
static char button_b_mapping[128] = "";

/* 
 * Array of property pointers for the 8 controller buttons:
 * 0: Up, 1: Down, 2: Left, 3: Right, 4: A, 5: B, 6: Select, 7: Start
 */
static struct hyrule_prop *controller_props[8] = { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL };

/* Corresponding device node names */
static const char *controller_names[] = { 
	"console/controller/up", 
	"console/controller/down", 
	"console/controller/left", 
	"console/controller/right", 
	"console/controller/a", 
	"console/controller/b",
	"console/controller/select",
	"console/controller/start"
};

/* Feedback messages shown when a button is "pressed" (read) */
static const char *controller_msgs[] = { 
	"Moved up\n", 
	"Moved down\n", 
	"Moved left\n", 
	"Moved right\n", 
	"A button pressed\n", 
	"B button pressed\n",
	"Select button pressed\n",
	"Start button pressed\n"
};

/* Movement vectors for the D-pad (Up, Down, Left, Right) */
static int controller_dx[] = { 0, 0, -1, 1 };
static int controller_dy[] = { -1, 1, 0, 0 };

/* Task for asynchronous update of available controller nodes */
static struct task controller_update_task;

static d_read_t hyrule_controller_read;
static d_write_t hyrule_controller_write;

/**
 * hyrule_controller_cdevsw - Device switch for all controller nodes.
 */
struct cdevsw hyrule_controller_cdevsw = {
	.d_version = D_VERSION,
	.d_open = hyrule_open,
	.d_close = hyrule_close,
	.d_read = hyrule_controller_read,
	.d_write = hyrule_controller_write,
	.d_name = "hyrule_controller",
};

/* State for the secret "cheat code" sequence */
static int h_idx = 0;
static const int h_seq[] = { 0, 0, 1, 1, 2, 3, 2, 3, 5, 4, 7 }; /* U, U, D, D, L, R, L, R, B, A, Start */

/**
 * check_combo_seq - Check if the current button press advances the cheat sequence.
 * @input: The index of the button pressed (0-7).
 */
static void
check_combo_seq(int input)
{
	if (input == h_seq[h_idx]) {
		h_idx++;
		if (h_idx == (sizeof(h_seq) / sizeof(h_seq[0]))) {
			printf("[HYRULE] Link feels a strange surge of power!\n");
			hyrule_invincible = 1;
			hyrule_update_status_nodes();
			h_idx = 0;
		}
	} else {
		/* Reset if the sequence is broken, but allow starting over from the beginning if it matches the first step */
		if (input == h_seq[0])
			h_idx = 1;
		else
			h_idx = 0;
	}
}

/**
 * hyrule_update_controller_nodes_task - Dynamic D-pad node management.
 *
 * This task is responsible for creating and destroying the up/down/left/right
 * device nodes based on whether the adjacent tiles on the map are accessible.
 * If Link is next to a wall, the corresponding direction node will disappear.
 */
static void
hyrule_update_controller_nodes_task(void *context, int pending)
{
	int x, y, i;
	struct hyrule_prop *to_remove[4] = { NULL, NULL, NULL, NULL };

	/* 
	 * We MUST take the lock here because we are modifying the nodes 
	 * that other threads might be reading/writing to.
	 */
	sx_xlock(&hyrule_sx);
	
	x = hyrule_get_prop_int("characters/link/location/x", 0);
	y = hyrule_get_prop_int("characters/link/location/y", 0);

	for (i = 0; i < 4; i++) {
		int nx = x + controller_dx[i];
		int ny = y + controller_dy[i];

		if (hyrule_map_is_accessible(nx, ny)) {
			if (controller_props[i] == NULL) {
				add_hyrule_node_custom(controller_names[i], "", &hyrule_controller_cdevsw);
				struct hyrule_prop *p;
				mtx_lock(&hyrule_mtx);
				LIST_FOREACH(p, &prop_list, next) {
					if (strcmp(p->name, controller_names[i]) == 0) {
						controller_props[i] = p;
						break;
					}
				}
				mtx_unlock(&hyrule_mtx);
			}
		} else {
			if (controller_props[i] != NULL) {
				/* 
				 * Collect nodes to remove and clear pointers while holding lock.
				 * Do NOT call remove_hyrule_node here as it calls destroy_dev
				 * which may sleep waiting for threads that are themselves
				 * waiting for this sx lock.
				 */
				to_remove[i] = controller_props[i];
				controller_props[i] = NULL;
			}
		}
	}
	sx_xunlock(&hyrule_sx);

	/* Actually destroy the nodes without holding the lock to avoid deadlock */
	for (i = 0; i < 4; i++) {
		if (to_remove[i] != NULL)
			remove_hyrule_node(to_remove[i]);
	}
}

/**
 * hyrule_update_controller_nodes - Schedule a controller node update.
 */
void
hyrule_update_controller_nodes(void)
{
	taskqueue_enqueue(taskqueue_thread, &controller_update_task);
}

/**
 * hyrule_controller_read - Handler for reading a controller node.
 *
 * Reading a node is equivalent to "pressing" the button.
 * - For D-pad: Moves Link and updates the map.
 * - For A/B: Executes the mapped item action (if any).
 */
static int
hyrule_controller_read(struct cdev *dev, struct uio *uio, int ioflag)
{
	struct hyrule_prop *p = dev->si_drv1;
	int i, dir = -1;

	if (uio->uio_offset > 0)
		return (0);

	for (i = 0; i < 8; i++) {
		if (p == controller_props[i]) {
			dir = i;
			break;
		}
	}

	if (dir == -1)
		return (ENXIO);

	sx_xlock(&hyrule_sx);
	if (!hyrule_is_active()) {
		sx_xunlock(&hyrule_sx);
		return (EACCES);
	}

	check_combo_seq(dir);

	if (dir < 4) {
		int nx = hyrule_get_prop_int("characters/link/location/x", 0) + controller_dx[dir];
		int ny = hyrule_get_prop_int("characters/link/location/y", 0) + controller_dy[dir];

		hyrule_set_prop_int("characters/link/location/x", nx);
		hyrule_set_prop_int("characters/link/location/y", ny);
		printf("[HYRULE] Link moved to (%d, %d) via %s\n", nx, ny, controller_names[dir]);
	} else {
		char *mapping = (dir == 4) ? button_a_mapping : button_b_mapping;
		if (mapping[0] != '\0') {
			struct hyrule_prop *prop = NULL;
			mtx_lock(&hyrule_mtx);
			LIST_FOREACH(prop, &prop_list, next) {
				if (strcmp(prop->name, mapping) == 0) {
					break;
				}
			}
			mtx_unlock(&hyrule_mtx);
			if (prop) {
				int error = uiomove(prop->value, strlen(prop->value), uio);
				sx_xunlock(&hyrule_sx);
				return (error);
			}
		}
		printf("[HYRULE] Button %s pressed\n", 
		    (dir == 4) ? "A" : (dir == 5) ? "B" : (dir == 6) ? "Select" : "Start");
	}
	
	sx_xunlock(&hyrule_sx);

	uiomove(__DECONST(char *, controller_msgs[dir]), strlen(controller_msgs[dir]), uio);
	
	if (dir < 4) {
		hyrule_update_controller_nodes();
		hyrule_update_local_nodes();
	}

	return (0);
}

/**
 * hyrule_controller_write - Handler for writing to a controller node.
 *
 * Writing to a node can have different effects:
 * - For D-pad: Same as reading (moves Link).
 * - For A/B: Maps the button to a specific item path.
 *   Example: `echo 'characters/link/items/sword' > /dev/hyrule/console/controller/a`
 */
static int
hyrule_controller_write(struct cdev *dev, struct uio *uio, int ioflag)
{
	struct hyrule_prop *p = dev->si_drv1;
	int i, dir = -1;

	for (i = 0; i < 8; i++) {
		if (p == controller_props[i]) {
			dir = i;
			break;
		}
	}

	if (dir == -1)
		return (ENXIO);

	sx_xlock(&hyrule_sx);
	if (!hyrule_is_active()) {
		sx_xunlock(&hyrule_sx);
		return (EACCES);
	}

	check_combo_seq(dir);

	if (dir < 4) {
		int nx = hyrule_get_prop_int("characters/link/location/x", 0) + controller_dx[dir];
		int ny = hyrule_get_prop_int("characters/link/location/y", 0) + controller_dy[dir];

		hyrule_set_prop_int("characters/link/location/x", nx);
		hyrule_set_prop_int("characters/link/location/y", ny);
		printf("[HYRULE] Link moved to (%d, %d) via write to %s\n", nx, ny, controller_names[dir]);
		uio->uio_resid = 0; /* Consume input */
	} else {
		char input[128];
		int error;
		int len = MIN(uio->uio_resid, sizeof(input) - 1);
		error = uiomove(input, len, uio);
		if (error) {
			sx_xunlock(&hyrule_sx);
			return (error);
		}
		input[len] = '\0';
		/* Trim newline and carriage return */
		while (len > 0 && (input[len-1] == '\n' || input[len-1] == '\r')) {
			input[--len] = '\0';
		}

		if (strncmp(input, "characters/link/items/", 22) == 0) {
			struct hyrule_prop *prop = NULL;
			mtx_lock(&hyrule_mtx);
			LIST_FOREACH(prop, &prop_list, next) {
				if (strcmp(prop->name, input) == 0) {
					break;
				}
			}
			mtx_unlock(&hyrule_mtx);
			if (prop) {
				char *mapping = (dir == 4) ? button_a_mapping : button_b_mapping;
				strlcpy(mapping, input, 128);
				printf("[HYRULE] Button %c mapped to %s\n", (dir == 4) ? 'A' : 'B', input);
			} else {
				printf("[HYRULE] Link doesn't have a %s to map!\n", input);
			}
		} else {
			printf("[HYRULE] Link doesn't know how to fight with a %s\n", input);
		}
	}
	
	sx_xunlock(&hyrule_sx);

	if (dir < 4) {
		hyrule_update_controller_nodes();
		hyrule_update_local_nodes();
	}

	return (0);
}

/**
 * hyrule_input_drain - Ensure all pending controller updates are finished.
 */
void
hyrule_input_drain(void)
{
	taskqueue_drain(taskqueue_thread, &controller_update_task);
}

/**
 * hyrule_input_init - Initialize the controller subsystem.
 */
void
hyrule_input_init(void)
{
	TASK_INIT(&controller_update_task, 0, hyrule_update_controller_nodes_task, NULL);

	/* Create static controller buttons */
	add_hyrule_node_custom("console/controller/a", "", &hyrule_controller_cdevsw);
	add_hyrule_node_custom("console/controller/b", "", &hyrule_controller_cdevsw);
	add_hyrule_node_custom("console/controller/select", "", &hyrule_controller_cdevsw);
	add_hyrule_node_custom("console/controller/start", "", &hyrule_controller_cdevsw);

	struct hyrule_prop *p;
	mtx_lock(&hyrule_mtx);
	LIST_FOREACH(p, &prop_list, next) {
		if (strcmp(p->name, "console/controller/a") == 0)
			controller_props[4] = p;
		else if (strcmp(p->name, "console/controller/b") == 0)
			controller_props[5] = p;
		else if (strcmp(p->name, "console/controller/select") == 0)
			controller_props[6] = p;
		else if (strcmp(p->name, "console/controller/start") == 0)
			controller_props[7] = p;
	}
	mtx_unlock(&hyrule_mtx);
}
