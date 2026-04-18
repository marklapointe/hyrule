# Hyrule Kernel Module - Development History

### Initial Architecture and Kernel Stability Challenges (2026-04-17 14:29 - 15:14)
The project initially explored a "Self-Unloading" model where the module would automatically unload when Link's health reached 0. This approach led to a series of "Supervisor read data" kernel panics during the unloading sequence.
- **Root Cause Analysis**: The panics were primarily caused by the kernel attempting to access module-resident data (such as process names) after the module had been unmapped. Race conditions between the unloading thread and the kernel process manager proved inherently risky in a self-unload scenario.
- **Decision**: To ensure absolute kernel stability, the self-unload logic was abandoned in favor of a robust "Game Console" metaphor.

### Transition to Game Console Metaphor (2026-04-17 15:14)
The module architecture was shifted to a "Virtual Console" model to provide a safer interaction layer.
- **Console Interface**: Added `/dev/hyrule/console/power` and `/dev/hyrule/console/reset` device nodes.
- **Power & Reset**: These nodes allow for explicit state management without risky memory unmapping. Powering off now blocks game interactions while keeping the module safely resident.
- **Cartridge System**: Added a simulated "Cartridge" system at `/dev/hyrule/console/cartridge` to simulate vintage hardware instability (e.g., "dusty" cartridges requiring a "blow" to function).

### Persistence: Save and Load System (2026-04-17 15:25)
A secure persistence layer was added to allow players to save and restore their game state.
- **Save/Load Devices**: Added `/dev/hyrule/game/save` and `/dev/hyrule/game/load`.
- **Security & Integrity**: Implemented a double-pass validation mechanism for the load operation. The module performs a non-destructive parse of the save file to verify format, property existence, and buffer boundaries before applying any changes to the kernel state.

### UI Improvements: Grid Visualization and Dynamic Movement (2026-04-17 15:40)
- **Enhanced Map**: The world map display was upgraded to a professional grid-based view with an integrated terrain legend.
- **Dynamic Movement**: Implemented dynamic directional nodes that appear and disappear based on Link's surroundings.
- **Taskqueue Integration**: Integrated the FreeBSD `taskqueue` system to handle dynamic device node creation and destruction safely.

### Controller Consolidation and Path Refinement (2026-04-17 15:58)
- **Controller Integration**: Reorganized all interaction devices under the `/dev/hyrule/console/controller/` path to better reflect the Game Console metaphor.
- **Button Support**: Added static `A` and `B` buttons to the controller for future action/combat features.
- **Path Cleanup**: Consolidated movement and action nodes into a single directory, removing the redundant `move` device from Link's location path.

### Local Exploration and Item Mapping (2026-04-17 16:15)
- **Local Entrances**: Added `/dev/hyrule/map/local/` for caves and shops. Link can now enter and exit these areas.
- **Starter Sword**: Added a script where Link meets an Old Knight in a cave and receives a wooden sword.
- **Item Equipment**: Implemented mapping for A and B buttons. Link can now equip items by writing their path to the button node.
- **Visualization**: Updated map legend and symbols, including 'W' for Woods and 'C' for Caves.
