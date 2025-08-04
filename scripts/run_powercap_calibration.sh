#!/bin/bash

# This script is used to run the powercap calibration for the Intel CPU.
# Uses a Fibonacci-based calibration program as an example.

# Turn-off the Wi-Fi
echo sudo ip link set wlp2s0 down

# Set the brightness to 0
sudo echo 0 | sudo tee "/sys/class/backlight/intel_backlight/brightness"

cd RAPL
make

# Powercap calibration: 2-25W range, 5°C variance, 10min timeout, 60s idle, 10 iterations
# Using compiled Fibonacci program with argument 20
sudo -E ./main --powercap-calibration 2-25 5 10 60 10 "gcc ../Utils/fibonacci.c -o fib" "./fib 20" C fibonacci
make clean

# Turn-on the Wi-Fi
sudo ip link set wlp2s0 up 

# Set the brightness to 2000
sudo echo 2000 | sudo tee "/sys/class/backlight/intel_backlight/brightness"