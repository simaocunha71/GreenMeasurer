#!/bin/bash

# This script is used to run the powercap calibration for the Intel CPU.

# Turn-off the Wi-Fi
echo sudo ip link set wlp2s0 down

# Set the brightness to 0
sudo echo 0 | sudo tee "/sys/class/backlight/intel_backlight/brightness"

cd RAPL
make
sudo -E ./main --powercap-calibration 2-25 5 10 60 10
make clean

# Turn-on the Wi-Fi
sudo ip link set wlp2s0 up 

# Set the brightness to 500
sudo echo 2000 | sudo tee "/sys/class/backlight/intel_backlight/brightness"