#!/usr/local/bin/bash

# Test for Konami Code and Invincibility in /dev/hyrule/

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
sudo kldload ./hyrule.ko
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

# 4. Check initial invincibility
echo "Checking initial invincibility status..."
cat $DEV_ROOT/characters/link/status/invincible | grep "0" > /dev/null
if [ $? -ne 0 ]; then
    echo "FAIL: Link should not be invincible yet"
    exit 1
fi

# 5. Set health to 0
echo "Setting health to 0..."
echo 0 | sudo tee $DEV_ROOT/characters/link/stats/health > /dev/null

# 6. Verify GAME OVER
echo "Verifying GAME OVER status..."
cat $DEV_ROOT/map/view | grep "GAME OVER" > /dev/null
if [ $? -ne 0 ]; then
    echo "FAIL: Map should show GAME OVER"
    exit 1
fi

# 7. Reset system
echo "Resetting system..."
echo 1 | sudo tee $DEV_ROOT/console/reset > /dev/null
sleep 1
echo blow | sudo tee $DEV_ROOT/console/cartridge > /dev/null
echo 1 | sudo tee $DEV_ROOT/console/power > /dev/null
sleep 1

# 8. Enter Konami Code
# Sequence: Up, Up, Down, Down, Left, Right, Left, Right, B, A, Start
# Link needs to be at a position where all directions are available and stay available
# during the sequence. (2,2) is a good starting point.
echo "Moving Link to (2,2) to enable Konami Code sequence..."
cat $DEV_ROOT/console/controller/down > /dev/null
cat $DEV_ROOT/console/controller/down > /dev/null
cat $DEV_ROOT/console/controller/right > /dev/null
cat $DEV_ROOT/console/controller/right > /dev/null
sleep 1

echo "Entering Konami Code..."
cat $DEV_ROOT/console/controller/up > /dev/null
cat $DEV_ROOT/console/controller/up > /dev/null
cat $DEV_ROOT/console/controller/down > /dev/null
cat $DEV_ROOT/console/controller/down > /dev/null
cat $DEV_ROOT/console/controller/left > /dev/null
cat $DEV_ROOT/console/controller/right > /dev/null
cat $DEV_ROOT/console/controller/left > /dev/null
cat $DEV_ROOT/console/controller/right > /dev/null
cat $DEV_ROOT/console/controller/b > /dev/null
cat $DEV_ROOT/console/controller/a > /dev/null
cat $DEV_ROOT/console/controller/start > /dev/null

# 9. Verify invincibility
echo "Verifying invincibility status..."
cat $DEV_ROOT/characters/link/status/invincible | grep "1" > /dev/null
if [ $? -ne 0 ]; then
    echo "FAIL: Link should be invincible now"
    exit 1
fi

# 10. Set health to 0 again
echo "Setting health to 0 while invincible..."
echo 0 | sudo tee $DEV_ROOT/characters/link/stats/health > /dev/null

# 11. Verify NO GAME OVER
echo "Verifying active status despite 0 HP..."
cat $DEV_ROOT/map/view | grep "Hyrule Map" > /dev/null
if [ $? -ne 0 ]; then
    echo "FAIL: Map should NOT show GAME OVER when invincible"
    exit 1
fi

echo "Konami Code and Invincibility tests PASSED!"
exit 0
