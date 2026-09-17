#!/bin/sh

cd "$(dirname "$0")"
./gametime.elf > ./log.txt 2>&1
