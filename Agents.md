# Agents of Hyrule

This file tracks the active agents (characters) and their current state in the Hyrulian Kernel Interface.

## Characters

### Link
- **Role**: Hero of Time / Protagonist
- **Stats**: Health, Stamina, Rupees
- **Properties**: Sword, Bow
- **Location**: Dynamically tracked on the 10x10 Map (`/dev/hyrule/map`)
- **Movements**: North, South, East, West (via `/dev/hyrule/characters/link/location/move`)
- **Special**: If health reaches 0, the kernel module unloads.

### Princess Zelda
- **Role**: Princess of Hyrule / Holder of Wisdom
- **Stats**: Health, Magic
- **Properties**: Holy Arrows

### Ganon
- **Role**: Demon King / Holder of Power / Antagonist
- **Stats**: Health, Power
- **Status**: Condition (ALIVE/SLAIN) tracked in `/dev/hyrule/characters/ganon/status/condition`

## Interaction
All agents are mapped to the `/dev/hyrule/characters/` directory. Their stats and status can be read via `cat` and modified via `echo` (where permitted by the kernel module logic).
