#!/bin/bash
./build.sh
sudo cp -r usr/ /
sudo systemd-hwdb update
sudo udevadm control -R
