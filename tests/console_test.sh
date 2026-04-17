#!/usr/local/bin/bash

# Test Console functionality (Power and Reset)

echo "--- Loading hyrule module ---"
sudo kldload ./hyrule.ko

# Check default state
echo "Initial Power: $(cat /dev/hyrule/console/power)"
echo "Initial Health: $(cat /dev/hyrule/characters/link/stats/health)"

# Test turning power off
echo "Turning power off..."
echo 0 > /dev/hyrule/console/power
echo "Power is now: $(cat /dev/hyrule/console/power)"
echo "Reading health (should be POWER OFF):"
cat /dev/hyrule/characters/link/stats/health

# Test writing while power is off
echo "Attempting to change health while power is off..."
echo 10 > /dev/hyrule/characters/link/stats/health 2>/dev/null
if [ $? -ne 0 ]; then
    echo "Successfully blocked write while power is off."
else
    echo "FAILED: Write allowed while power is off."
fi

# Test turning power on (should reset)
echo "Changing health to 5 (will be reset when power turned on)..."
# We need power on to change health
echo 1 > /dev/hyrule/console/power
echo 5 > /dev/hyrule/characters/link/stats/health
echo "Health is now: $(cat /dev/hyrule/characters/link/stats/health)"

echo "Turning power off then on..."
echo 0 > /dev/hyrule/console/power
echo 1 > /dev/hyrule/console/power
echo "Health (should be 3 again): $(cat /dev/hyrule/characters/link/stats/health)"

# Test Reset button
echo "Changing health to 10..."
echo 10 > /dev/hyrule/characters/link/stats/health
echo "Health is now: $(cat /dev/hyrule/characters/link/stats/health)"
echo "Pressing reset button..."
echo 1 > /dev/hyrule/console/reset
echo "Health (should be 3 again): $(cat /dev/hyrule/characters/link/stats/health)"

# Test Cartridge
echo "Checking cartridge state: $(cat /dev/hyrule/console/cartridge)"
echo "Reading health (might be GLITCH if dusty):"
cat /dev/hyrule/characters/link/stats/health

echo "Blowing on cartridge..."
echo "blow" > /dev/hyrule/console/cartridge
echo "Cartridge is now: $(cat /dev/hyrule/console/cartridge)"

echo "Reading health (should be 3):"
cat /dev/hyrule/characters/link/stats/health

# Test Game Over
echo "Setting health to 0..."
echo 0 > /dev/hyrule/characters/link/stats/health
echo "Reading health (should be GAME OVER):"
cat /dev/hyrule/characters/link/stats/health

echo "--- Unloading hyrule module ---"
sudo kldunload hyrule
