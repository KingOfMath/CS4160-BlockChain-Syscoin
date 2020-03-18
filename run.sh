#!/bin/bash
meson . build
ninja -C build
cd build/gateway
./gateway
