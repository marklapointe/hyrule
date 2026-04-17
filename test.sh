#!/bin/sh
echo "--- Starting Test Session: $(date) ---" >> last.log
make clean >> last.log 2>&1
make >> last.log 2>&1
if [ $? -eq 0 ]; then
    echo "Build successful." >> last.log
    cd tests
    sudo kyua test >> ../last.log 2>&1
    echo "Tests completed with exit code $?." >> ../last.log
else
    echo "Build failed." >> last.log
fi
echo "--- End of Test Session ---" >> last.log
