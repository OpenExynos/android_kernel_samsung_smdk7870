#!/bin/bash

export ARCH=arm64
export CROSS_COMPILE=../aarch64-linux-android-4.9/bin/aarch64-linux-android-
export ANDROID_MAJOR_VERSION=mm

# kernel
make $1
make -j$(nproc --all)

# dts
mkdir arch/arm64/boot/tmp

IFS='_'
read -a data <<< "$1"

mv arch/arm64/boot/dts/*${data[0]}_*.dtb arch/arm64/boot/tmp/
./tools/dtbtool arch/arm64/boot/tmp/ -o arch/arm64/boot/dtb
rm -rf arch/arm64/boot/tmp
