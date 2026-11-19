#!/bin/sh
set -eu

amixer -c 0 sset "ADCL Input MIC1 Boost" on
amixer -c 0 sset "MIC1 gain volume" 31
amixer -c 0 sset "ADC volume" 160,160
