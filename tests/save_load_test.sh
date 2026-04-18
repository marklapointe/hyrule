#!/bin/sh
# Test Save/Load functionality

# Ensure module is loaded
sudo kldload ./hyrule.ko 2>/dev/null

# Make sure cartridge is clean for consistent results
echo "blow" > /dev/hyrule/console/cartridge
echo 1 > /dev/hyrule/console/power

# 1. Change some state
echo "10" > /dev/hyrule/characters/link/stats/rupees
cat /dev/hyrule/console/controller/down > /dev/null
cat /dev/hyrule/console/controller/right > /dev/null

# Check state before save
R1=$(cat /dev/hyrule/characters/link/stats/rupees | tr -d '\n')
X1=$(cat /dev/hyrule/characters/link/location/x | tr -d '\n')
Y1=$(cat /dev/hyrule/characters/link/location/y | tr -d '\n')
echo "Before Save: Rupees=$R1, X=$X1, Y=$Y1"

# 2. Save
cat /dev/hyrule/game/save > game.sav
echo "Save file created."

# 3. Reset or change state again
echo 1 > /dev/hyrule/console/reset
echo "Reset performed."

# Check state after reset
R2=$(cat /dev/hyrule/characters/link/stats/rupees | tr -d '\n')
X2=$(cat /dev/hyrule/characters/link/location/x | tr -d '\n')
Y2=$(cat /dev/hyrule/characters/link/location/y | tr -d '\n')
echo "After Reset: Rupees=$R2, X=$X2, Y=$Y2"

ls -l /dev/hyrule/game/load || echo "LOAD DEVICE MISSING!"

# 4. Load
cat game.sav > /dev/hyrule/game/load
echo "Load performed."

# Check state after load
R3=$(cat /dev/hyrule/characters/link/stats/rupees | tr -d '\n')
X3=$(cat /dev/hyrule/characters/link/location/x | tr -d '\n')
Y3=$(cat /dev/hyrule/characters/link/location/y | tr -d '\n')
echo "After Load: Rupees=$R3, X=$X3, Y=$Y3"

# Verify
if [ "$R1" = "$R3" ] && [ "$X1" = "$X3" ] && [ "$Y1" = "$Y3" ]; then
    echo "SUCCESS: Save/Load worked!"
else
    echo "FAILURE: State mismatch!"
    sudo kldunload hyrule
    exit 1
fi

# 5. Test Map Config
echo "wwwwwwwwww" > /dev/hyrule/world/map_config
cat /dev/hyrule/game/save > map.sav
# Change map back
for i in 1 2 3 4 5 6 7 8 9 10; do echo "ffffffffff"; done > /dev/hyrule/world/map_config
cat map.sav > /dev/hyrule/game/load
MAP=$(head -n 1 /dev/hyrule/world/map_config | tr -d '\n')
if [ "$MAP" = "wwwwwwwwww" ]; then
    echo "SUCCESS: Map Save/Load worked!"
else
    echo "FAILURE: Map state mismatch! Got: $MAP"
    sudo kldunload hyrule
    exit 1
fi

sudo kldunload hyrule
rm game.sav map.sav
