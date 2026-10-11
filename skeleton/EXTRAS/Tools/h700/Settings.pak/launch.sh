#!/bin/sh

cd "$(dirname "$0")" || exit 1
./settings.elf > settings.log 2>&1
