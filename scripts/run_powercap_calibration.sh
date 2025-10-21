#!/bin/bash

# This script is used to run the powercap calibration for the Intel CPU.
# Uses a Fibonacci-based calibration program as an example.

# Turn-off the Wi-Fi
echo sudo ip link set wlo1 down

cd RAPL
make

# Powercap calibration: 2-25W range, 5°C variance, 4hr timeout, 60s idle, 10 iterations
# Using compiled Fibonacci program with argument 45
sudo -E ./main --powercap-calibration 2-25 5 10 60 4 "gcc ../Utils/fibonacci.c -o fib" "./fib 45" C fibonacci
make clean

# Turn-on the Wi-Fi
sudo ip link set wlo1 up 