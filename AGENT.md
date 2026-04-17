# Agents of Hyrule

This file tracks the active agents (characters) and their current state in the Hyrulian Kernel Interface.

## Characters

### Link
- **Role**: Hero of Time / Protagonist
- **Stats**: Health, Stamina, Rupees
- **Items**: Sword, Bow, Bombs
- **Location**: Dynamically tracked on the 10x10 Map (`/dev/hyrule/map`). Can enter local landmarks like Caves 'C' and Shops 'S'.
- **Controller**: Use directional nodes (`up`, `down`, `left`, `right`) and map items to action buttons (`a`, `b`) in `/dev/hyrule/console/controller/`.
- **Special**: If health reaches 0, the game enters a "GAME OVER" state.

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
