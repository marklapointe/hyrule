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

/* Map State */
static char world_map[MAP_SIZE][MAP_SIZE];
static char world_entrances[MAP_SIZE][MAP_SIZE];

/* Local nodes under /dev/hyrule/map/local/ */
static struct hyrule_prop *local_entrance_node = NULL;
static struct hyrule_prop *local_exit_node = NULL;
static struct hyrule_prop *local_next_node = NULL;
static struct hyrule_prop *local_prev_node = NULL;
static struct hyrule_prop *local_treasure_node = NULL;
static struct hyrule_prop *local_boss_node = NULL;
static struct hyrule_prop *local_room_node = NULL;
static int inside_entrance = 0;
static char current_entrance_type[32] = "";

static struct task local_update_task;
static d_read_t hyrule_local_read;
static d_write_t hyrule_local_write;
static d_close_t hyrule_local_close;

struct cdevsw hyrule_local_cdevsw = {
	.d_version = D_VERSION,
	.d_open = hyrule_open,
	.d_close = hyrule_local_close,
	.d_read = hyrule_local_read,
	.d_write = hyrule_local_write,
	.d_name = "hyrule_local",
};

static void
hyrule_update_local_nodes_task(void *context, int pending)
{
	struct hyrule_prop *to_remove[16] = { NULL };
	int tr_idx = 0;

	sx_xlock(&hyrule_sx);
	int x = hyrule_get_prop_int("characters/link/location/x", 0);
	int y = hyrule_get_prop_int("characters/link/location/y", 0);
	char entrance = 0;

	if (x >= 0 && x < MAP_SIZE && y >= 0 && y < MAP_SIZE)
		entrance = world_entrances[y][x];

	if (inside_entrance) {
		/* We're inside */
		if (local_entrance_node != NULL) {
			to_remove[tr_idx++] = local_entrance_node;
			local_entrance_node = NULL;
		}

		int dungeon = hyrule_get_prop_int("characters/link/location/dungeon", 0);
		int room = hyrule_get_prop_int("characters/link/location/room", 0);

		/* Exit node only in room 0 (Entrance) */
		if (room == 0) {
			if (local_exit_node == NULL) {
				add_hyrule_node_custom("map/local/exit", "", &hyrule_local_cdevsw);
				struct hyrule_prop *p;
				mtx_lock(&hyrule_mtx);
				LIST_FOREACH(p, &prop_list, next) {
					if (strcmp(p->name, "map/local/exit") == 0) {
						local_exit_node = p;
						break;
					}
				}
				mtx_unlock(&hyrule_mtx);
			}
		} else {
			if (local_exit_node != NULL) {
				to_remove[tr_idx++] = local_exit_node;
				local_exit_node = NULL;
			}
		}

		if (dungeon > 0) {
			/* Room description node */
			if (local_room_node == NULL) {
				add_hyrule_node_custom("map/local/room", "", &hyrule_local_cdevsw);
				struct hyrule_prop *p;
				mtx_lock(&hyrule_mtx);
				LIST_FOREACH(p, &prop_list, next) {
					if (strcmp(p->name, "map/local/room") == 0) {
						local_room_node = p;
						break;
					}
				}
				mtx_unlock(&hyrule_mtx);
			}

			/* Dungeon specific nodes */
			if (room < 9) {
				if (local_next_node == NULL) {
					add_hyrule_node_custom("map/local/next", "", &hyrule_local_cdevsw);
					struct hyrule_prop *p;
					mtx_lock(&hyrule_mtx);
					LIST_FOREACH(p, &prop_list, next) {
						if (strcmp(p->name, "map/local/next") == 0) {
							local_next_node = p;
							break;
						}
					}
					mtx_unlock(&hyrule_mtx);
				}
			} else {
				if (local_next_node != NULL) {
					to_remove[tr_idx++] = local_next_node;
					local_next_node = NULL;
				}
			}

			if (room > 0) {
				if (local_prev_node == NULL) {
					add_hyrule_node_custom("map/local/prev", "", &hyrule_local_cdevsw);
					struct hyrule_prop *p;
					mtx_lock(&hyrule_mtx);
					LIST_FOREACH(p, &prop_list, next) {
						if (strcmp(p->name, "map/local/prev") == 0) {
							local_prev_node = p;
							break;
						}
					}
					mtx_unlock(&hyrule_mtx);
				}
			} else {
				if (local_prev_node != NULL) {
					to_remove[tr_idx++] = local_prev_node;
					local_prev_node = NULL;
				}
			}

			if (room == 4) {
				int treasures = hyrule_get_prop_int("world/dungeon/treasures_found", 0);
				if (!(treasures & (1 << (dungeon - 1)))) {
					if (local_treasure_node == NULL) {
						add_hyrule_node_custom("map/local/treasure", "", &hyrule_local_cdevsw);
						struct hyrule_prop *p;
						mtx_lock(&hyrule_mtx);
						LIST_FOREACH(p, &prop_list, next) {
							if (strcmp(p->name, "map/local/treasure") == 0) {
								local_treasure_node = p;
								break;
							}
						}
						mtx_unlock(&hyrule_mtx);
					}
				} else {
					if (local_treasure_node != NULL) {
						to_remove[tr_idx++] = local_treasure_node;
						local_treasure_node = NULL;
					}
				}
			} else {
				if (local_treasure_node != NULL) {
					to_remove[tr_idx++] = local_treasure_node;
					local_treasure_node = NULL;
				}
			}

			if (room == 9) {
				int bosses = hyrule_get_prop_int("world/dungeon/bosses_defeated", 0);
				if (!(bosses & (1 << (dungeon - 1)))) {
					if (local_boss_node == NULL) {
						add_hyrule_node_custom("map/local/boss", "", &hyrule_local_cdevsw);
						struct hyrule_prop *p;
						mtx_lock(&hyrule_mtx);
						LIST_FOREACH(p, &prop_list, next) {
							if (strcmp(p->name, "map/local/boss") == 0) {
								local_boss_node = p;
								break;
							}
						}
						mtx_unlock(&hyrule_mtx);
					}
				} else {
					if (local_boss_node != NULL) {
						to_remove[tr_idx++] = local_boss_node;
						local_boss_node = NULL;
					}
				}
			} else {
				if (local_boss_node != NULL) {
					to_remove[tr_idx++] = local_boss_node;
					local_boss_node = NULL;
				}
			}
		}
	} else {
		/* We're outside */
		if (local_exit_node != NULL) {
			to_remove[tr_idx++] = local_exit_node;
			local_exit_node = NULL;
		}
		if (local_next_node != NULL) {
			to_remove[tr_idx++] = local_next_node;
			local_next_node = NULL;
		}
		if (local_prev_node != NULL) {
			to_remove[tr_idx++] = local_prev_node;
			local_prev_node = NULL;
		}
		if (local_treasure_node != NULL) {
			to_remove[tr_idx++] = local_treasure_node;
			local_treasure_node = NULL;
		}
		if (local_boss_node != NULL) {
			to_remove[tr_idx++] = local_boss_node;
			local_boss_node = NULL;
		}
		if (local_room_node != NULL) {
			to_remove[tr_idx++] = local_room_node;
			local_room_node = NULL;
		}

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

			if (local_entrance_node == NULL || strcmp(local_entrance_node->name, path) != 0) {
				if (local_entrance_node != NULL) {
					to_remove[tr_idx++] = local_entrance_node;
					local_entrance_node = NULL;
				}
				add_hyrule_node_custom(path, "", &hyrule_local_cdevsw);
				struct hyrule_prop *p;
				mtx_lock(&hyrule_mtx);
				LIST_FOREACH(p, &prop_list, next) {
					if (strcmp(p->name, path) == 0) {
						local_entrance_node = p;
						break;
					}
				}
				mtx_unlock(&hyrule_mtx);
				strlcpy(current_entrance_type, ename, sizeof(current_entrance_type));
			}
		} else {
			if (local_entrance_node != NULL) {
				to_remove[tr_idx++] = local_entrance_node;
				local_entrance_node = NULL;
			}
		}
	}
	sx_xunlock(&hyrule_sx);

	/* Actually destroy the nodes outside the lock to avoid deadlock */
	for (int i = 0; i < tr_idx; i++) {
		if (to_remove[i] != NULL)
			remove_hyrule_node(to_remove[i]);
	}
}

void
hyrule_update_local_nodes(void)
{
	taskqueue_enqueue(taskqueue_thread, &local_update_task);
}


static int
hyrule_local_read(struct cdev *dev, struct uio *uio, int ioflag)
{
	struct hyrule_prop *p = dev->si_drv1;
	char msg[512] = "";
	int msg_len;

	if (uio->uio_offset > 0)
		return (0);

	sx_xlock(&hyrule_sx);
	if (!hyrule_is_active()) {
		sx_xunlock(&hyrule_sx);
		return (EACCES);
	}

	if (p == local_entrance_node) {
		if (strcmp(current_entrance_type, "cave") == 0) {
			int has_sword = 0;
			struct hyrule_prop *item;
			
			snprintf(msg, sizeof(msg), "Old Knight: IT'S DANGEROUS TO GO ALONE! TAKE THIS. You received the WOODEN SWORD!\n");
			
			mtx_lock(&hyrule_mtx);
			LIST_FOREACH(item, &prop_list, next) {
				if (strcmp(item->name, "characters/link/items/sword") == 0) {
					if (strcmp(item->value, "None\n") != 0) {
						has_sword = 1;
					} else {
						strlcpy(item->value, "Wooden Sword\n", sizeof(item->value));
						has_sword = 1;
					}
					break;
				}
			}
			mtx_unlock(&hyrule_mtx);

			if (!has_sword) {
				add_hyrule_node("characters/link/items/sword", "Wooden Sword\n");
			}
			hyrule_set_prop_int("characters/link/stats/sword_level", 0);
			inside_entrance = 1;
		} else if (strcmp(current_entrance_type, "upgrade") == 0) {
			int triforces = hyrule_get_prop_int("objects/triforce/parts/collected", 0);
			int sword_level = hyrule_get_prop_int("characters/link/stats/sword_level", 0);
			
			if (triforces >= 2 && sword_level < 2) {
				snprintf(msg, sizeof(msg), "Master Knight: You have 2 Triforces. You have mastered the sword. Take the Master Sword!\n");
				struct hyrule_prop *item;
				mtx_lock(&hyrule_mtx);
				LIST_FOREACH(item, &prop_list, next) {
					if (strcmp(item->name, "characters/link/items/sword") == 0) {
						strlcpy(item->value, "Master Sword\n", sizeof(item->value));
						break;
					}
				}
				mtx_unlock(&hyrule_mtx);
				hyrule_set_prop_int("characters/link/stats/sword_level", 2);
			} else if (triforces >= 1 && sword_level < 1) {
				snprintf(msg, sizeof(msg), "White Knight: You have 1 Triforce. Take the White Sword!\n");
				struct hyrule_prop *item;
				mtx_lock(&hyrule_mtx);
				LIST_FOREACH(item, &prop_list, next) {
					if (strcmp(item->name, "characters/link/items/sword") == 0) {
						strlcpy(item->value, "White Sword\n", sizeof(item->value));
						break;
					}
				}
				mtx_unlock(&hyrule_mtx);
				hyrule_set_prop_int("characters/link/stats/sword_level", 1);
			} else {
				snprintf(msg, sizeof(msg), "Knight: Return when you have mastered it (collect more Triforces).\n");
			}
			inside_entrance = 1;
		} else if (strncmp(current_entrance_type, "dungeon", 7) == 0 || strcmp(current_entrance_type, "ganon") == 0) {
			int dungeon_id = 0;
			if (strcmp(current_entrance_type, "ganon") == 0) {
				dungeon_id = 4;
				int triforces = hyrule_get_prop_int("objects/triforce/parts/collected", 0);
				if (triforces < 3) {
					snprintf(msg, sizeof(msg), "The gate to Ganon's Castle is sealed. You need all 3 Triforces.\n");
					sx_xunlock(&hyrule_sx);
					uiomove(msg, strlen(msg), uio);
					return (0);
				}
			} else {
				dungeon_id = current_entrance_type[7] - '0';
			}
			
			snprintf(msg, sizeof(msg), "Entering %s...\n", current_entrance_type);
			inside_entrance = 1;
			hyrule_set_prop_int("characters/link/location/dungeon", dungeon_id);
			hyrule_set_prop_int("characters/link/location/room", 0);
		} else {
			snprintf(msg, sizeof(msg), "Entering %s...\n", current_entrance_type);
			inside_entrance = 1;
		}
	} else if (p == local_exit_node) {
		inside_entrance = 0;
		hyrule_set_prop_int("characters/link/location/dungeon", 0);
		hyrule_set_prop_int("characters/link/location/room", 0);
		snprintf(msg, sizeof(msg), "Exiting to the world map...\n");
	} else if (p == local_room_node) {
		int dungeon = hyrule_get_prop_int("characters/link/location/dungeon", 0);
		int room = hyrule_get_prop_int("characters/link/location/room", 0);
		const char *dname = (dungeon == 4) ? "Ganon's Castle" : "Dungeon";
		
		if (room == 0) snprintf(msg, sizeof(msg), "%s Entrance\n", dname);
		else if (room == 4) snprintf(msg, sizeof(msg), "Treasure Room of %s %d\n", dname, dungeon);
		else if (room == 9) snprintf(msg, sizeof(msg), "Boss Room of %s %d\n", dname, dungeon);
		else snprintf(msg, sizeof(msg), "Dungeon %d, Room %d: A dark and damp room.\n", dungeon, room);
	} else if (p == local_next_node) {
		int room = hyrule_get_prop_int("characters/link/location/room", 0);
		if (room < 9) {
			room++;
			hyrule_set_prop_int("characters/link/location/room", room);
			snprintf(msg, sizeof(msg), "Moving forward to room %d...\n", room);
		}
	} else if (p == local_prev_node) {
		int room = hyrule_get_prop_int("characters/link/location/room", 0);
		if (room > 0) {
			room--;
			hyrule_set_prop_int("characters/link/location/room", room);
			snprintf(msg, sizeof(msg), "Moving back to room %d...\n", room);
		}
	} else if (p == local_treasure_node) {
		int dungeon = hyrule_get_prop_int("characters/link/location/dungeon", 0);
		int treasures = hyrule_get_prop_int("world/dungeon/treasures_found", 0);
		
		if (!(treasures & (1 << (dungeon - 1)))) {
			treasures |= (1 << (dungeon - 1));
			hyrule_set_prop_int("world/dungeon/treasures_found", treasures);
			
			const char *weapon = "New Weapon";
			if (dungeon == 1) weapon = "Boomerang";
			else if (dungeon == 2) weapon = "Raft";
			else if (dungeon == 3) weapon = "Stepladder";
			
			snprintf(msg, sizeof(msg), "You found the %s!\n", weapon);
		} else {
			snprintf(msg, sizeof(msg), "The treasure chest is empty.\n");
		}
	} else if (p == local_boss_node) {
		int dungeon = hyrule_get_prop_int("characters/link/location/dungeon", 0);
		int bosses = hyrule_get_prop_int("world/dungeon/bosses_defeated", 0);
		
		if (!(bosses & (1 << (dungeon - 1)))) {
			bosses |= (1 << (dungeon - 1));
			hyrule_set_prop_int("world/dungeon/bosses_defeated", bosses);
			
			if (dungeon <= 3) {
				int triforces = hyrule_get_prop_int("objects/triforce/parts/collected", 0);
				triforces++;
				hyrule_set_prop_int("objects/triforce/parts/collected", triforces);
				snprintf(msg, sizeof(msg), "Boss defeated! You got a Triforce piece! Total: %d\n", triforces);
			} else {
				snprintf(msg, sizeof(msg), "Ganon defeated! You have saved Hyrule!\n");
				hyrule_set_prop_int("characters/ganon/status/condition", 0); /* DEAD */
			}
		} else {
			snprintf(msg, sizeof(msg), "The boss is already defeated.\n");
		}
	} else {
		sx_xunlock(&hyrule_sx);
		return (ENXIO);
	}

	sx_xunlock(&hyrule_sx);
	
	msg_len = strlen(msg);
	uiomove(msg, msg_len, uio);
	
	return (0);
}

static int
hyrule_local_write(struct cdev *dev, struct uio *uio, int ioflag)
{
	struct hyrule_prop *p = dev->si_drv1;

	sx_xlock(&hyrule_sx);
	if (!hyrule_is_active()) {
		sx_xunlock(&hyrule_sx);
		return (EACCES);
	}

	if (p == local_entrance_node) {
		if (strncmp(current_entrance_type, "dungeon", 7) == 0 || strcmp(current_entrance_type, "ganon") == 0) {
			int dungeon_id = (strcmp(current_entrance_type, "ganon") == 0) ? 4 : current_entrance_type[7] - '0';
			if (dungeon_id == 4) {
				int triforces = hyrule_get_prop_int("objects/triforce/parts/collected", 0);
				if (triforces < 3) {
					sx_xunlock(&hyrule_sx);
					return (EPERM);
				}
			}
			hyrule_set_prop_int("characters/link/location/dungeon", dungeon_id);
			hyrule_set_prop_int("characters/link/location/room", 0);
		}
		inside_entrance = 1;
		printf("[HYRULE] Entering %s via write\n", current_entrance_type);
	} else if (p == local_exit_node) {
		inside_entrance = 0;
		hyrule_set_prop_int("characters/link/location/dungeon", 0);
		hyrule_set_prop_int("characters/link/location/room", 0);
		printf("[HYRULE] Exiting via write\n");
	} else if (p == local_next_node) {
		int room = hyrule_get_prop_int("characters/link/location/room", 0);
		if (room < 9) {
			room++;
			hyrule_set_prop_int("characters/link/location/room", room);
		}
	} else if (p == local_prev_node) {
		int room = hyrule_get_prop_int("characters/link/location/room", 0);
		if (room > 0) {
			room--;
			hyrule_set_prop_int("characters/link/location/room", room);
		}
	} else if (p == local_treasure_node) {
		int dungeon = hyrule_get_prop_int("characters/link/location/dungeon", 0);
		int treasures = hyrule_get_prop_int("world/dungeon/treasures_found", 0);
		treasures |= (1 << (dungeon - 1));
		hyrule_set_prop_int("world/dungeon/treasures_found", treasures);
	} else if (p == local_boss_node) {
		int dungeon = hyrule_get_prop_int("characters/link/location/dungeon", 0);
		int bosses = hyrule_get_prop_int("world/dungeon/bosses_defeated", 0);
		bosses |= (1 << (dungeon - 1));
		hyrule_set_prop_int("world/dungeon/bosses_defeated", bosses);
		if (dungeon <= 3) {
			int triforces = hyrule_get_prop_int("objects/triforce/parts/collected", 0);
			triforces++;
			hyrule_set_prop_int("objects/triforce/parts/collected", triforces);
		}
	} else if (p == local_room_node) {
		/* No-op */
	} else {
		sx_xunlock(&hyrule_sx);
		return (ENXIO);
	}

	sx_xunlock(&hyrule_sx);
	
	uio->uio_resid = 0;

	return (0);
}

static int
hyrule_local_close(struct cdev *dev, int fflag, int devtype, struct thread *td)
{
	hyrule_update_local_nodes();
	hyrule_update_controller_nodes();
	return (0);
}

/* Symbols for display */
static char
get_map_symbol(char type)
{
	switch (type) {
	case 'f': return ('.'); /* Field */
	case 'W': return ('W'); /* Woods */
	case 'e': return (' '); /* Empty */
	case 'a': return ('F'); /* Fairy */
	case 'c': return ('C'); /* Cave */
	case 's': return ('S'); /* Shop */
	case '1': return ('1');
	case '2': return ('2');
	case '3': return ('3');
	case 'g': return ('G');
	case 'u': return ('U');
	default:  return (type);
	}
}

int
hyrule_map_is_accessible(int x, int y)
{
	if (inside_entrance)
		return (0);
	if (x < 0 || x >= MAP_SIZE || y < 0 || y >= MAP_SIZE)
		return (0);
	/* Lowercase or digits are accessible, uppercase is blocked */
	char c = world_map[y][x];
	return (islower(c) || isdigit(c));
}

/* Display /dev/hyrule/map */
static int
hyrule_map_read(struct cdev *dev, struct uio *uio, int ioflag)
{
	char buf[1536];
	int pos = 0;
	int x, y;

	sx_slock(&hyrule_sx);

	if (!hyrule_is_active()) {
		const char *msg = (hyrule_power == 0) ? "POWER OFF\n" : "GAME OVER\n";
		int msg_len = strlen(msg);
		if (uio->uio_offset >= msg_len) {
			sx_sunlock(&hyrule_sx);
			return (0);
		}
		int error = uiomove(__DECONST(char *, msg) + uio->uio_offset, msg_len - uio->uio_offset, uio);
		sx_sunlock(&hyrule_sx);
		return (error);
	}

	int lx = hyrule_get_prop_int("characters/link/location/x", 0);
	int ly = hyrule_get_prop_int("characters/link/location/y", 0);

	pos += snprintf(buf + pos, sizeof(buf) - pos, "--- Hyrule Map ---\n");
	if (inside_entrance) {
		pos += snprintf(buf + pos, sizeof(buf) - pos, "Inside %s at (%d, %d)\n\n", current_entrance_type, lx, ly);
	}

	for (y = 0; y < MAP_SIZE; y++) {
		/* Row border */
		for (x = 0; x < MAP_SIZE; x++) {
			pos += snprintf(buf + pos, sizeof(buf) - pos, "+---");
		}
		pos += snprintf(buf + pos, sizeof(buf) - pos, "+\n");

		/* Row content */
		for (x = 0; x < MAP_SIZE; x++) {
			if (x == lx && y == ly)
				pos += snprintf(buf + pos, sizeof(buf) - pos, "| L ");
			else
				pos += snprintf(buf + pos, sizeof(buf) - pos, "| %c ", get_map_symbol(world_map[y][x]));
		}
		pos += snprintf(buf + pos, sizeof(buf) - pos, "|\n");
	}
	/* Final border */
	for (x = 0; x < MAP_SIZE; x++) {
		pos += snprintf(buf + pos, sizeof(buf) - pos, "+---");
	}
	pos += snprintf(buf + pos, sizeof(buf) - pos, "+\n");

	pos += snprintf(buf + pos, sizeof(buf) - pos, "Link at: (%d, %d)\n\n", lx, ly);
	pos += snprintf(buf + pos, sizeof(buf) - pos, "Legend: L: Link, .: Field, W: Woods, 1-3: Dungeons, G: Ganon, F: Fairy, C: Cave, S: Shop, U: Upgrade\n");

	if (uio->uio_offset >= pos) {
		sx_sunlock(&hyrule_sx);
		return (0);
	}

	int error = uiomove(buf + uio->uio_offset, pos - uio->uio_offset, uio);
	sx_sunlock(&hyrule_sx);
	return (error);
}

/* Read/Write /dev/hyrule/world/map_config */
void
hyrule_map_get_config(char *buf, size_t size)
{
	int x, y, pos = 0;

	for (y = 0; y < MAP_SIZE; y++) {
		for (x = 0; x < MAP_SIZE; x++) {
			if (pos < size - 1)
				buf[pos++] = world_map[y][x];
		}
		if (pos < size - 1)
			buf[pos++] = '\n';
	}
	buf[pos] = '\0';
}

void
hyrule_map_set_config(const char *input, size_t len)
{
	int x = 0, y = 0, i;

	for (i = 0; i < len && y < MAP_SIZE; i++) {
		if (isspace(input[i])) continue;
		world_map[y][x] = input[i];
		x++;
		if (x >= MAP_SIZE) {
			x = 0;
			y++;
		}
	}
	hyrule_update_controller_nodes();
}

static int
hyrule_map_config_read(struct cdev *dev, struct uio *uio, int ioflag)
{
	char buf[128];
	int x, y, pos = 0;

	sx_slock(&hyrule_sx);

	if (!hyrule_is_active()) {
		const char *msg = (hyrule_power == 0) ? "POWER OFF\n" : "GAME OVER\n";
		int msg_len = strlen(msg);
		if (uio->uio_offset >= msg_len) {
			sx_sunlock(&hyrule_sx);
			return (0);
		}
		int error = uiomove(__DECONST(char *, msg) + uio->uio_offset, msg_len - uio->uio_offset, uio);
		sx_sunlock(&hyrule_sx);
		return (error);
	}

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

	if (!hyrule_power) {
		sx_xunlock(&hyrule_sx);
		return (EACCES);
	}

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
	hyrule_update_controller_nodes();
	return (0);
}


void
hyrule_map_drain(void)
{
	taskqueue_drain(taskqueue_thread, &local_update_task);
}

void
hyrule_map_init(void)
{
	int x, y;

	TASK_INIT(&local_update_task, 0, hyrule_update_local_nodes_task, NULL);

	/* Initialize map with some fields and a dungeon */
	for (y = 0; y < MAP_SIZE; y++) {
		for (x = 0; x < MAP_SIZE; x++) {
			world_map[y][x] = 'f';
			world_entrances[y][x] = 0;
		}
	}
	world_map[2][2] = '1'; /* Dungeon 1 at (2,2) */
	world_map[2][7] = '2'; /* Dungeon 2 at (7,2) */
	world_map[7][2] = '3'; /* Dungeon 3 at (2,7) */
	world_map[5][5] = 'g'; /* Ganon's Castle at (5,5) */
	world_map[9][9] = 'u'; /* Upgrade Cave at (9,9) */
	world_map[5][0] = 'W'; /* Blocked woods at (0,5) */
	world_entrances[0][0] = 'c'; /* Cave at (0,0) */
	world_entrances[2][2] = '1';
	world_entrances[2][7] = '2';
	world_entrances[7][2] = '3';
	world_entrances[5][5] = 'g';
	world_entrances[9][9] = 'u';
	
	hyrule_update_local_nodes();
}

/* 
 * Character device operations for the map.
 */
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

