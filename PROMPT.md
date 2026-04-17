### Directives and Accomplishments

#### Directives
1.  **Initial Goal**: Create a silly kernel module named `hyrule`.
2.  **Fix Kernel Panic**: Resolve the "bad si_name" panic by using `make_dev_s` with `MAKEDEV_CHECKNAME`.
3.  **Generic Device Tree**: Implement a hierarchical device structure under `/dev/hyrule/` for characters and objects.
4.  **Error Checking & Safety**: Add locks (`sx`, `mtx`) and input validation to prevent panics and race conditions.
5.  **Character Stats**: Add health, stamina, magic, and rupees for Link and Zelda.
6.  **Help Device**: Create `/dev/hyrule/help` for user instructions.
7.  **Documentation**: Add a FreeBSD man page (`hyrule.4`) in mdoc format.
8.  **Testing**: Develop a suite of automated tests using the FreeBSD ATF framework.
9.  **Tracking**: Maintain this `PROMPT.md` file to record progress.
10. **External Relocation**: Move the module source from `/usr/src/sys/modules/hyrule` to `/home/mlapointe/git/hyrule` to separate it from the base system.
11. **Installation Support**: Ensure the `Makefile` supports system-wide installation.
12. **Project Documentation**: Create a `README.md` with instructions and a `LICENSE` file with a 3-Clause BSD license.
13. **Clean Repo**: Add a `.gitignore` to keep compiled objects out of the repository.
14. **Map System**: Implement a 10x10 world map with editable terrain and movement logic.
15. **Code Modularization**: Split the codebase into multiple files (`hyrule.h`, `hyrule.c`, `hyrule_map.c`) for better maintainability.
16. **Character Tracking**: Create `Agents.md` to document the roles and states of characters.
17. **Death Unload**: Unload the module automatically when Link's health reaches 0.
18. **100% Code Coverage**: Expand the test suite to cover all logic paths.

#### Accomplishments
-   **Robust Device Creation**: Safely creates nested paths like `/dev/hyrule/characters/link/stats/health`.
-   **Thread-Safe Access**: Uses `sx` locks for read/write operations and `mtx` for list management.
-   **Informative Help System**: `/dev/hyrule/help` is accessible via `cat` and provides usage examples.
-   **Expanded Gameplay Mechanics**: Characters now have stateful properties that can be interacted with.
-   **Integrated Documentation**: The man page is integrated into the build system and follows standard FreeBSD conventions.
-   **Automated Verification**: Comprehensive test suite covers existence, read/write correctness, offsets, and boundary limits.
-   **Clean Lifecycle**: Module loading and unloading is fully handled with safe memory management.
-   **System Isolation**: Successfully relocated the project to a user-owned directory (`/home/mlapointe/git/hyrule`), decoupled from the core kernel source tree.
-   **Installation Ready**: Verified that `make install` handles module and man page deployment.
-   **Project Metadata**: Added `README.md`, `LICENSE` (3-Clause BSD), and `.gitignore` to maintain a professional project structure.
-   **Dynamic World Map**: Implemented a 10x10 grid with real-time Link position tracking and terrain-based movement blocks.
-   **Modular Architecture**: Successfully refactored the module into separate components for core logic and map management.
-   **Character Management**: Added `Agents.md` and restored full character stats for Link, Zelda, and Ganon.
-   **Death Unload System**: Implemented a taskqueue-based self-unload mechanism triggered by Link's death.
-   **Comprehensive Testing**: Expanded the ATF test suite to 16 tests, achieving high code coverage for core features.
