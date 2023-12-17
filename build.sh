#!/bin/bash
if [ ! -d usr/bin ]; then
	mkdir usr/bin
fi
gcc HandyGCCS++.cpp -o usr/bin/handycon -levdev -ludev -lstdc++
