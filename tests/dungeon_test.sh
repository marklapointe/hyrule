#!/usr/local/bin/bash

# Test for Dungeons and Sword Upgrades in /dev/hyrule/

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

# Helper to wait for device node
wait_for_dev() {
    for i in {1..20}; do
        if [ -c "$1" ]; then return 0; fi
        sleep 0.1
    done
    return 1
}

# 4. Get Wooden Sword first
echo "Getting Wooden Sword from Cave at (0,0)..."
wait_for_dev $DEV_ROOT/map/local/cave
cat $DEV_ROOT/map/local/cave > /dev/null
wait_for_dev $DEV_ROOT/map/local/exit
cat $DEV_ROOT/map/local/exit > /dev/null
sleep 0.2

# 5. Move to Dungeon 1 at (2,2)
echo "Moving to Dungeon 1 (2,2)..."
# (0,0) -> (2,0) -> (2,2)
echo right | sudo tee $DEV_ROOT/console/controller/right > /dev/null
echo right | sudo tee $DEV_ROOT/console/controller/right > /dev/null
echo down | sudo tee $DEV_ROOT/console/controller/down > /dev/null
echo down | sudo tee $DEV_ROOT/console/controller/down > /dev/null

# 6. Enter Dungeon 1
echo "Entering Dungeon 1..."
wait_for_dev $DEV_ROOT/map/local/dungeon1
cat $DEV_ROOT/map/local/dungeon1 > /dev/null

# 7. Navigate to Treasure Room (4)
echo "Navigating to Treasure Room..."
for i in {1..4}; do
    wait_for_dev $DEV_ROOT/map/local/next
    cat $DEV_ROOT/map/local/next > /dev/null
done

wait_for_dev $DEV_ROOT/map/local/treasure
cat $DEV_ROOT/map/local/treasure | grep "Boomerang"
if [ $? -ne 0 ]; then
    echo "FAIL: Did not find Boomerang in Dungeon 1"
    exit 1
fi

# 8. Navigate to Boss Room (9)
echo "Navigating to Boss Room..."
for i in {5..9}; do
    wait_for_dev $DEV_ROOT/map/local/next
    cat $DEV_ROOT/map/local/next > /dev/null
done

wait_for_dev $DEV_ROOT/map/local/boss
cat $DEV_ROOT/map/local/boss | grep "Triforce"
if [ $? -ne 0 ]; then
    echo "FAIL: Did not get Triforce from Boss"
    exit 1
fi

# 9. Exit Dungeon 1
echo "Navigating back to exit..."
for i in {1..9}; do
    wait_for_dev $DEV_ROOT/map/local/prev
    cat $DEV_ROOT/map/local/prev > /dev/null
done
wait_for_dev $DEV_ROOT/map/local/exit
cat $DEV_ROOT/map/local/exit > /dev/null
sleep 0.2

# 10. Check White Sword Upgrade at (9,9)
echo "Moving to Upgrade Cave (9,9)..."
# We are at (2,2)
for i in {1..7}; do echo right | sudo tee $DEV_ROOT/console/controller/right > /dev/null; done
for i in {1..7}; do echo down | sudo tee $DEV_ROOT/console/controller/down > /dev/null; done

echo "Entering Upgrade Cave for White Sword..."
wait_for_dev $DEV_ROOT/map/local/upgrade
RESULT=$(cat $DEV_ROOT/map/local/upgrade)
echo "$RESULT"
if [[ "$RESULT" != *"White Sword"* ]]; then
    echo "FAIL: Did not receive White Sword with 1 Triforce"
    exit 1
fi
cat $DEV_ROOT/map/local/exit > /dev/null
sleep 0.2

# 11. Move to Dungeon 2 at (7,2)
echo "Moving to Dungeon 2 (7,2)..."
# We are at (9,9) -> (7,9) -> (7,2)
for i in {1..2}; do echo left | sudo tee $DEV_ROOT/console/controller/left > /dev/null; done
for i in {1..7}; do echo up | sudo tee $DEV_ROOT/console/controller/up > /dev/null; done

echo "Entering Dungeon 2..."
wait_for_dev $DEV_ROOT/map/local/dungeon2
cat $DEV_ROOT/map/local/dungeon2 > /dev/null

# 12. Complete Dungeon 2
echo "Completing Dungeon 2..."
for i in {1..4}; do wait_for_dev $DEV_ROOT/map/local/next; cat $DEV_ROOT/map/local/next > /dev/null; done
wait_for_dev $DEV_ROOT/map/local/treasure
cat $DEV_ROOT/map/local/treasure | grep "Raft"
for i in {5..9}; do wait_for_dev $DEV_ROOT/map/local/next; cat $DEV_ROOT/map/local/next > /dev/null; done
wait_for_dev $DEV_ROOT/map/local/boss
cat $DEV_ROOT/map/local/boss | grep "Triforce"
for i in {1..9}; do wait_for_dev $DEV_ROOT/map/local/prev; cat $DEV_ROOT/map/local/prev > /dev/null; done
wait_for_dev $DEV_ROOT/map/local/exit
cat $DEV_ROOT/map/local/exit > /dev/null
sleep 0.2

# 13. Get Master Sword Upgrade at (9,9)
echo "Moving to Upgrade Cave (9,9)..."
# We are at (7,2) -> (9,2) -> (9,9)
for i in {1..2}; do echo right | sudo tee $DEV_ROOT/console/controller/right > /dev/null; done
for i in {1..7}; do echo down | sudo tee $DEV_ROOT/console/controller/down > /dev/null; done

echo "Entering Upgrade Cave for Master Sword..."
wait_for_dev $DEV_ROOT/map/local/upgrade
RESULT=$(cat $DEV_ROOT/map/local/upgrade)
echo "$RESULT"
if [[ "$RESULT" != *"Master Sword"* ]]; then
    echo "FAIL: Did not receive Master Sword with 2 Triforces"
    exit 1
fi
cat $DEV_ROOT/map/local/exit > /dev/null
sleep 0.2

# 14. Move to Ganon's Castle (5,5)
echo "Moving to Ganon's Castle (5,5)..."
# We are at (9,9) -> (5,9) -> (5,5)
for i in {1..4}; do echo left | sudo tee $DEV_ROOT/console/controller/left > /dev/null; done
for i in {1..4}; do echo up | sudo tee $DEV_ROOT/console/controller/up > /dev/null; done

echo "Trying to enter Ganon's Castle with 2 Triforces..."
wait_for_dev $DEV_ROOT/map/local/ganon
RESULT=$(cat $DEV_ROOT/map/local/ganon)
echo "$RESULT"
if [[ "$RESULT" != *"sealed"* ]]; then
    echo "FAIL: Should not be able to enter Ganon's Castle with only 2 Triforces"
    exit 1
fi

# 15. Complete Dungeon 3 at (2,7)
echo "Moving to Dungeon 3 (2,7)..."
# We are at (5,5) -> (2,5) -> (2,7)
for i in {1..3}; do echo left | sudo tee $DEV_ROOT/console/controller/left > /dev/null; done
for i in {1..2}; do echo down | sudo tee $DEV_ROOT/console/controller/down > /dev/null; done

echo "Completing Dungeon 3..."
wait_for_dev $DEV_ROOT/map/local/dungeon3
cat $DEV_ROOT/map/local/dungeon3 > /dev/null
for i in {1..4}; do wait_for_dev $DEV_ROOT/map/local/next; cat $DEV_ROOT/map/local/next > /dev/null; done
wait_for_dev $DEV_ROOT/map/local/treasure
cat $DEV_ROOT/map/local/treasure | grep "Stepladder"
for i in {5..9}; do wait_for_dev $DEV_ROOT/map/local/next; cat $DEV_ROOT/map/local/next > /dev/null; done
wait_for_dev $DEV_ROOT/map/local/boss
cat $DEV_ROOT/map/local/boss | grep "Triforce"
for i in {1..9}; do wait_for_dev $DEV_ROOT/map/local/prev; cat $DEV_ROOT/map/local/prev > /dev/null; done
wait_for_dev $DEV_ROOT/map/local/exit
cat $DEV_ROOT/map/local/exit > /dev/null
sleep 0.2

# 16. Enter and Complete Ganon's Castle at (5,5)
echo "Moving to Ganon's Castle (5,5)..."
# We are at (2,7) -> (5,7) -> (5,5)
for i in {1..3}; do echo right | sudo tee $DEV_ROOT/console/controller/right > /dev/null; done
for i in {1..2}; do echo up | sudo tee $DEV_ROOT/console/controller/up > /dev/null; done

echo "Entering Ganon's Castle with 3 Triforces..."
wait_for_dev $DEV_ROOT/map/local/ganon
cat $DEV_ROOT/map/local/ganon > /dev/null
for i in {1..9}; do wait_for_dev $DEV_ROOT/map/local/next; cat $DEV_ROOT/map/local/next > /dev/null; done
wait_for_dev $DEV_ROOT/map/local/boss
cat $DEV_ROOT/map/local/boss | grep "Ganon defeated"

echo "Dungeon and Upgrade tests PASSED!"
exit 0
