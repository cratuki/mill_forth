#!/bin/bash

set -e

make

echo "--------------------------------------------------------"
#valgrind --leak-check=full ./exe
./exe

