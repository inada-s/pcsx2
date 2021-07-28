#!/bin/bash
set -eux

SCRIPT_DIR=$(cd $(dirname $0); pwd)
cd $SCRIPT_DIR

#ee-addr2line  ee-c++        ee-g++        ee-gcov       ee-objcopy    ee-readelf    ee-strip
#ee-ar         ee-c++filt    ee-gcc        ee-ld         ee-objdump    ee-size
#ee-as         ee-cpp        ee-gccbug     ee-nm         ee-ranlib     ee-strings

PATH=$PATH:/usr/local/bin
export MSYS_NO_PATHCONV=1
#readonly PS2DEV_DOCKER=ps2dev/ps2dev:v1.3.0
#readonly EE_PREFIX="mips64r5900el-ps2-elf-"
readonly PS2DEV_DOCKER=asscore/ps2dev-docker
readonly EE_PREFIX="ee-"

docker run -v $(pwd):$(pwd) \
    $PS2DEV_DOCKER ${EE_PREFIX}gcc -O0 -G0 \
    $(pwd)/src/main.c \
    -c -o $(pwd)/bin/main.x

docker run -v $(pwd):$(pwd) \
    $PS2DEV_DOCKER ${EE_PREFIX}ld \
    -T $(pwd)/src/ld.script \
    $(pwd)/bin/main.x \
    -o $(pwd)/bin/main.o

docker run -v $(pwd):$(pwd) \
    $PS2DEV_DOCKER ${EE_PREFIX}objdump \
    -h $(pwd)/bin/main.o

docker run -v $(pwd):$(pwd) \
    $PS2DEV_DOCKER ${EE_PREFIX}objcopy \
    --only-section gdx.data \
    --only-section gdx.func \
    --only-section gdx.hook \
    $(pwd)/bin/main.o \
    $(pwd)/bin/gdxpatch.o

docker run -v $(pwd):$(pwd) \
    $PS2DEV_DOCKER ${EE_PREFIX}objdump \
    -D $(pwd)/bin/gdxpatch.o > $(pwd)/bin/gdxpatch.asm
