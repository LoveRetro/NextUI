#!/bin/sh

cd "$(dirname "$0")"
./battery.elf > ./log.txt 2>&1
