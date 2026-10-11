#!/bin/sh
# Map stock RGXX_MODEL (from dmenu.bin) to DEVICE.
# Behaviour keys on DEVICE; RGXX_MODEL is for display/logs only.
#
#   RG28xx     rg28xx
#   RG34xx     rg34xx
#   RG34xxSP   rg34xxsp
#   RG35xx     rg35xxplus   (Plus and 2024 both report RG35xx)
#   RG35xxH    rg35xxh
#   RG35xxPro  rg35xxpro
#   RG35xxSP   rg35xxsp
#   RG40xxH    rg40xxh
#   RG40xxV    rg40xxv
#   RGcubexx   rgcubexx
#   RGSP       rgsp
#   anything else / empty → rg35xxplus

h700_export_device() {
	if [ -z "$RGXX_MODEL" ]; then
		RGXX_MODEL="$(strings /mnt/vendor/bin/dmenu.bin 2>/dev/null | grep -m1 '^RG')"
	fi
	export RGXX_MODEL

	# Longer suffixes before shorter / bare forms. Case-sensitive stock strings.
	case "$RGXX_MODEL" in
		RG28xx) DEVICE=rg28xx ;;
		RG34xxSP) DEVICE=rg34xxsp ;;
		RG34xx) DEVICE=rg34xx ;;
		RGSP) DEVICE=rgsp ;;
		RG35xxSP) DEVICE=rg35xxsp ;;
		RG35xxPro) DEVICE=rg35xxpro ;;
		RG35xxH) DEVICE=rg35xxh ;;
		RG35xx) DEVICE=rg35xxplus ;;
		RG40xxV) DEVICE=rg40xxv ;;
		RG40xxH) DEVICE=rg40xxh ;;
		RGcubexx) DEVICE=rgcubexx ;;
		*) DEVICE=rg35xxplus ;;
	esac
	export DEVICE
}
