#!/bin/sh
# Test Safe Load functionality (Negative tests)

sudo kldload ./hyrule.ko 2>/dev/null
echo "blow" > /dev/hyrule/console/cartridge

echo "--- Running Safe Load Tests ---"

# Set a known state first
echo "77" > /dev/hyrule/characters/link/stats/rupees

# Test 1: Invalid property
printf "PROP:invalid/prop\nSIZE:1\nX\n\n" > fail.sav
cat fail.sav > /dev/hyrule/game/load 2>/dev/null
if [ $? -ne 0 ]; then
    echo "PASS: Invalid property rejected"
else
    echo "FAIL: Invalid property accepted"
    RET=1
fi

# Test 2: Mismatched size (content too short)
printf "PROP:characters/link/stats/rupees\nSIZE:10\n3\n\n" > fail.sav
cat fail.sav > /dev/hyrule/game/load 2>/dev/null
if [ $? -ne 0 ]; then
    echo "PASS: Mismatched size (short) rejected"
else
    echo "FAIL: Mismatched size (short) accepted"
    RET=1
fi

# Test 3: Garbage data
printf "This is not a save file" > fail.sav
cat fail.sav > /dev/hyrule/game/load 2>/dev/null
if [ $? -ne 0 ]; then
    echo "PASS: Garbage data rejected"
else
    echo "FAIL: Garbage data accepted"
    RET=1
fi

# Test 4: Too large size
printf "PROP:characters/link/stats/rupees\nSIZE:99999\n3\n\n" > fail.sav
cat fail.sav > /dev/hyrule/game/load 2>/dev/null
if [ $? -ne 0 ]; then
    echo "PASS: Too large size rejected"
else
    echo "FAIL: Too large size accepted"
    RET=1
fi

# Verify state is UNCHANGED after failures
RUPEES=$(cat /dev/hyrule/characters/link/stats/rupees | tr -d '\n')
if [ "$RUPEES" = "77" ]; then
    echo "PASS: State remained unchanged after failed loads"
else
    echo "FAIL: State was corrupted by failed load! (Rupees=$RUPEES)"
    RET=1
fi

# Test 5: Valid load after failures
printf "PROP:characters/link/stats/rupees\nSIZE:3\n10\n\n" > success.sav
cat success.sav > /dev/hyrule/game/load 2>/dev/null
RUPEES=$(cat /dev/hyrule/characters/link/stats/rupees | tr -d '\n')
if [ "$RUPEES" = "10" ]; then
    echo "PASS: Valid load worked after failures"
else
    echo "FAIL: Valid load failed after rejections (Rupees=$RUPEES)"
    RET=1
fi

sudo kldunload hyrule
rm fail.sav success.sav
exit ${RET:-0}
