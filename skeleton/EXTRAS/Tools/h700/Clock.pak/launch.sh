#!/bin/sh

cd "$(dirname "$0")"
./clock.elf > ./log.txt 2>&1
