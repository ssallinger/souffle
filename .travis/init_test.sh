#!/bin/bash


# Enable debugging and logging of shell operations
# that are executed.
set -e
set -x

# configure project

# create configure files
./bootstrap
./configure
make -j2


set +e
set +x
