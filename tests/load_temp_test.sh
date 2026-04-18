#!/usr/local/bin/bash

# Test for automatic loading of coretemp and amdtemp modules by hyrule.ko

echo "=== Testing Temperature Module Loading ==="

# 1. Unload modules if already loaded
echo "Cleaning up..."
sudo kldunload coretemp 2>/dev/null
sudo kldunload amdtemp 2>/dev/null
sudo kldunload hyrule 2>/dev/null

# 2. Check they are not loaded
if kldstat | grep -q "coretemp"; then
    echo "FAILED: coretemp still loaded"
    exit 1
fi
if kldstat | grep -q "amdtemp"; then
    echo "FAILED: amdtemp still loaded"
    exit 1
fi

# 3. Load hyrule.ko
echo "Loading hyrule.ko..."
sudo kldload ./hyrule.ko
if [ $? -ne 0 ]; then
    echo "FAILED: could not load hyrule.ko"
    exit 1
fi

# 4. Check if coretemp and amdtemp are now loaded
echo "Checking coretemp and amdtemp status..."
CORE_LOADED=0
AMD_LOADED=0

if kldstat | grep -q "coretemp"; then
    echo "SUCCESS: coretemp is loaded"
    CORE_LOADED=1
else
    echo "FAILED: coretemp is NOT loaded"
fi

if kldstat | grep -q "amdtemp"; then
    echo "SUCCESS: amdtemp is loaded"
    AMD_LOADED=1
else
    echo "FAILED: amdtemp is NOT loaded"
fi

# 5. Cleanup
echo "Unloading hyrule.ko..."
sudo kldunload hyrule.ko

if [ $CORE_LOADED -eq 1 ] && [ $AMD_LOADED -eq 1 ]; then
    echo "=== All temperature module loading tests PASSED! ==="
    exit 0
else
    echo "=== Some tests FAILED! ==="
    exit 1
fi
