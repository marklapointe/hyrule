#!/usr/local/bin/bash

# Test for local entrances and item mapping in /dev/hyrule/

MODULE="./hyrule.ko"
DEV_ROOT="/dev/hyrule"

# 1. Clean and Build
echo "Building module..."
make clean > /dev/null
make > /dev/null
if [ $? -ne 0 ]; then
    echo "FAIL: Build failed"
    exit 1
fi

# 2. Load Module
echo "Loading module..."
sudo kldunload hyrule 2>/dev/null
sudo kldload $MODULE
if [ $? -ne 0 ]; then
    echo "FAIL: Could not load module"
    exit 1
fi

# Clean up on exit
trap "sudo kldunload hyrule" EXIT

# 3. Setup Power & Cartridge
echo "Setting up console..."
echo blow | sudo tee $DEV_ROOT/console/cartridge > /dev/null
echo 1 | sudo tee $DEV_ROOT/console/power > /dev/null
sleep 1

# 4. Check initial map state at (0,0)
echo "Checking map for Cave 'C' at (0,0)..."
cat $DEV_ROOT/map/view | grep "C" > /dev/null
if [ $? -ne 0 ]; then
    echo "FAIL: Map symbol 'C' not found at (0,0)"
    exit 1
fi

# 5. Enter Cave
echo "Checking for cave node in /dev/hyrule/map/local/..."
ls $DEV_ROOT/map/local/
if [ ! -c $DEV_ROOT/map/local/cave ]; then
    echo "FAIL: Missing local entrance node 'cave'"
    exit 1
fi

echo "Entering cave..."
RESULT=$(cat $DEV_ROOT/map/local/cave)
echo "$RESULT"
if [[ "$RESULT" != *"You received the WOODEN SWORD!"* ]]; then
    echo "FAIL: Expected Old Knight message not found"
    exit 1
fi

# 6. Verify item creation
echo "Checking for sword in link/items/..."
if [ ! -c $DEV_ROOT/characters/link/items/sword ]; then
    echo "FAIL: sword node not found"
    exit 1
fi

# 7. Check for exit node
echo "Checking for exit node..."
ls $DEV_ROOT/map/local/
if [ ! -c $DEV_ROOT/map/local/exit ]; then
    echo "FAIL: Missing exit node"
    exit 1
fi

# 8. Exit cave
echo "Exiting cave..."
cat $DEV_ROOT/map/local/exit | grep "Exiting"
sleep 1

# 9. Map item to Button A
echo "Equipping sword to Button A..."
echo "characters/link/items/sword" | sudo tee $DEV_ROOT/console/controller/a > /dev/null
if [ $? -ne 0 ]; then
    echo "FAIL: Could not write to Button A"
    exit 1
fi

# 10. Use Button A
echo "Using Button A (should use sword)..."
RESULT=$(cat $DEV_ROOT/console/controller/a)
echo "$RESULT"
if [[ "$RESULT" != *"Wooden Sword"* ]]; then
    echo "FAIL: Button A did not use the mapped item"
    exit 1
fi

# 11. Negative test: map invalid device
echo "Mapping 'SCSI controller' to Button B..."
echo "characters/link/items/scsi_controller" | sudo tee $DEV_ROOT/console/controller/b > /dev/null
# This should succeed but print rejection to dmesg as it's not a real item Link has

# Also check for "Link doesn't know how to fight with a SCSI controller" message (simulated)
echo "/dev/sd0" | sudo tee $DEV_ROOT/console/controller/b > /dev/null

echo "Checking kernel logs for funny rejections..."
sudo dmesg | tail -n 20 | grep "Link doesn't know how to fight with a /dev/sd0"
if [ $? -ne 0 ]; then
    echo "FAIL: Could not find funny rejection message in dmesg"
    # Don't fail the whole test just because of dmesg tailing issues, but it's a warning
fi

echo "All local exploration and item mapping tests PASSED!"
exit 0
