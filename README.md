# Hyrule Kernel Module

A fun, educational FreeBSD kernel module that brings the land of Hyrule to your `/dev` directory.

## Description

The `hyrule` module is a character device driver that creates a hierarchical structure under `/dev/hyrule/`. You can interact with various characters and objects from "The Legend of Zelda" series by reading and writing to virtual device nodes.

**Warning**: This is an educational project and should never be used on production systems. Modifying kernel space can cause system panics if not handled correctly.

## Features

- **Hierarchical Device Tree**: Devices are organized by type and name, such as `/dev/hyrule/characters/link/stats/health`.
- **Character Stats**: Read and update attributes like health, stamina, magic, and rupees for characters like Link and Zelda.
- **Help System**: A built-in help device at `/dev/hyrule/help` provides usage instructions.
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

The project includes an ATF (Automated Testing Framework) suite. To run the tests:

```bash
cd tests
sudo kyua test
```

## License

This project is licensed under the 3-Clause BSD License. See the [LICENSE](LICENSE) file for details.

## Fair Use Notice

Characters and themes from "The Legend of Zelda" are trademarks and copyrights of Nintendo. This software is an educational project and its use of these characters is intended as fair use.

## Author

Mark LaPointe <mark@cloudbsd.org>
