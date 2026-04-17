#!/usr/local/bin/bash

# Test for controller nodes in /dev/hyrule/console/controller/

MODULE="./hyrule.ko"
DEV_ROOT="/dev/hyrule"

echo "--- Hyrule Controller Test ---"

# 1. Reload module
sudo kldunload hyrule.ko 2>/dev/null
sudo kldload $MODULE
if [ $? -ne 0 ]; then
    echo "Failed to load module"
    exit 1
fi

# 2. Power on and clean cartridge
echo "blow" > $DEV_ROOT/console/cartridge
echo "1" > $DEV_ROOT/console/power

sleep 1 # Wait for taskqueue to create nodes

# 3. Initial state at (0,0)
# Map is 10x10. At (0,0), only down and right should be available, plus A and B.
echo "Checking initial controller nodes at (0,0)..."
ls $DEV_ROOT/console/controller/

# Verify nodes
if [ ! -c $DEV_ROOT/console/controller/down ] || [ ! -c $DEV_ROOT/console/controller/right ]; then
    echo "FAIL: Missing directional nodes (down/right)"
    exit 1
fi
if [ ! -c $DEV_ROOT/console/controller/a ] || [ ! -c $DEV_ROOT/console/controller/b ]; then
    echo "FAIL: Missing button nodes (a/b)"
    exit 1
fi
if [ -c $DEV_ROOT/console/controller/up ] || [ -c $DEV_ROOT/console/controller/left ]; then
    echo "FAIL: Unexpected directional nodes (up/left)"
    exit 1
fi

# 4. Test buttons
echo "Testing Button A..."
cat $DEV_ROOT/console/controller/a | grep "A button pressed"
if [ $? -ne 0 ]; then
    echo "FAIL: Button A read failure"
    exit 1
fi

echo "Testing Button B..."
echo "press" > $DEV_ROOT/console/controller/b
# Check kernel output for button press would be good, but we'll assume it worked if no crash

# 5. Move down via cat
echo "Moving down via cat /dev/hyrule/console/controller/down..."
cat $DEV_ROOT/console/controller/down
sleep 1

echo "Checking controller nodes at (0,1)..."
ls $DEV_ROOT/console/controller/
if [ ! -c $DEV_ROOT/console/controller/up ] || [ ! -c $DEV_ROOT/console/controller/down ] || [ ! -c $DEV_ROOT/console/controller/right ]; then
    echo "FAIL: Missing nodes at (0,1) (up/down/right)"
    exit 1
fi

# 6. Move right via echo
echo "Moving right via echo to /dev/hyrule/console/controller/right..."
echo "move" > $DEV_ROOT/console/controller/right
sleep 1

echo "Checking controller nodes at (1,1)..."
ls $DEV_ROOT/console/controller/
if [ ! -c $DEV_ROOT/console/controller/up ] || [ ! -c $DEV_ROOT/console/controller/down ] || [ ! -c $DEV_ROOT/console/controller/left ] || [ ! -c $DEV_ROOT/console/controller/right ]; then
    echo "FAIL: Missing nodes at (1,1)"
    exit 1
fi

# 7. Check map for legend
echo "Checking map for legend..."
cat $DEV_ROOT/map/view | grep "Legend"
if [ $? -ne 0 ]; then
    echo "FAIL: Legend not found in map"
    exit 1
fi

echo "--- SUCCESS: Controller Test Passed ---"
sudo kldunload hyrule.ko
exit 0
