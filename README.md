# Hyrule Kernel Module

A fun, educational FreeBSD kernel module that brings the land of Hyrule to your `/dev` directory.

## Description

The `hyrule` module is a character device driver that creates a hierarchical structure under `/dev/hyrule/`. You can interact with various characters and objects from "The Legend of Zelda" series by reading and writing to virtual device nodes.

**Warning**: This is an educational project and should never be used on production systems. Modifying kernel space can cause system panics if not handled correctly.

## Features

- **Hierarchical Device Tree**: Devices are organized by type and name, such as `/dev/hyrule/characters/link/stats/health`.
- **Game Console Interface**: Manage the module via `/dev/hyrule/console/power` and `/dev/hyrule/console/reset`.
- **Cartridge System**: Simulates hardware instability. Use `echo blow > /dev/hyrule/console/cartridge` to stabilize the system.
- **Local Map Exploration**: Discover and enter caves, shops, and multi-room dungeons (Dungeon 1, 2, 3, and Ganon's Castle) via `/dev/hyrule/map/local/`.
- **Dungeon Progression**: Fight bosses to collect Triforce pieces and find treasures like the Boomerang and Raft.
- **Sword Upgrades**: Visit the Upgrade Cave to improve your sword (Wooden, White, Master Sword) as you collect Triforce pieces.
- **Item Equipment**: Link can now find items like the `sword` and equip them to the Game Console controller's A and B buttons.
- **Save & Load**: Persist your game state via `/dev/hyrule/game/save` and `/dev/hyrule/game/load`.
- **Enhanced Map System**: A 10x10 grid accessible via `/dev/hyrule/map/view`, featuring detailed terrain and Link's position.
- **Controller Interface**: Use the Game Console controller nodes in `/dev/hyrule/console/controller/` to interact. Directional nodes (`up`, `down`, `left`, `right`) appear dynamically when movement is possible. Action buttons (`a`, `b`) are always available.
- **Map Configuration**: Customize the world map by writing a grid of characters to `/dev/hyrule/world/map_config`. Lowercase characters are accessible, while uppercase characters block Link's path.
- **Character Stats**: Read and update attributes like health, stamina, magic, and rupees for characters like Link and Zelda.
- **Manual Page**: Comprehensive documentation accessible via `man hyrule`.

## Installation

To build and install the module and its manual page:

1.  **Build**:
    ```bash
    make
    ```

2.  **Install**:
    ```bash
    sudo make install
    ```
    This installs the `hyrule.ko` module to `/boot/modules/` and the manual page to `/usr/share/man/man4/`.

## Usage

### Loading the Module
```bash
sudo kldload hyrule
```

### Exploring Hyrule
List the available device nodes:
```bash
ls -R /dev/hyrule
```

### Console and Cartridge
Power on the system (if dusty, it might fail):
```bash
echo "1" | sudo tee /dev/hyrule/console/power
```

If it fails, blow on the cartridge:
```bash
echo "blow" | sudo tee /dev/hyrule/console/cartridge
echo "1" | sudo tee /dev/hyrule/console/power
```

### Map and Movement
View the map:
```bash
cat /dev/hyrule/map
```

Move Link east:
```bash
cat /dev/hyrule/console/controller/right
```

### Save and Load
Save current progress:
```bash
cat /dev/hyrule/game/save > my_adventure.sav
```

Load previous progress:
```bash
sudo dd if=my_adventure.sav of=/dev/hyrule/game/load
```
*Note: Using `dd` or redirecting directly into the device node works for loading.*

### Reading Stats
Check Link's health:
```bash
cat /dev/hyrule/characters/link/stats/health
```

### Updating Stats
Restore Link's health:
```bash
echo "3 hearts" | sudo tee /dev/hyrule/characters/link/stats/health
```

### Help
Get instructions directly from the kernel:
```bash
cat /dev/hyrule/help
```

### Manual Page
Read the full documentation:
```bash
man hyrule
```

## Testing

The project includes a suite of shell-based tests. To run them:

```bash
cd tests
sudo ./console_test.sh
sudo ./save_load_test.sh
sudo ./safe_load_test.sh
```

## License

This project is licensed under the 3-Clause BSD License. See the [LICENSE](LICENSE) file for details.

## Fair Use Notice

Characters and themes from "The Legend of Zelda" are trademarks and copyrights of Nintendo. This software is an educational project and its use of these characters is intended as fair use.

## Author

Mark LaPointe <mark@cloudbsd.org>
