#!/bin/sh

cd "$(dirname "$0")"
./bootlogo.elf > ./log.txt 2>&1
