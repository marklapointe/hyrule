#!/usr/local/bin/bash

# Main test runner for Hyrule Kernel Module

echo "=== Hyrule Full Test Suite ==="

# Cleanup
sudo kldunload hyrule 2>/dev/null

# Build
make clean && make
if [ $? -ne 0 ]; then
    echo "Build failed"
    exit 1
fi

# Run tests
# We run them with sudo bash to ensure redirections work and they have root access
sudo bash tests/console_test.sh
sudo bash tests/save_load_test.sh
sudo bash tests/safe_load_test.sh
sudo bash tests/controller_test.sh
sudo bash tests/local_item_test.sh
sudo bash tests/konami_test.sh
sudo bash tests/dungeon_test.sh

echo "=== All tests completed ==="
