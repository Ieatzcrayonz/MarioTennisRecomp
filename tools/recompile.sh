#!/bin/sh
# Regenerate everything the build consumes from the ROM. Run from the project root:
#   sh tools/recompile.sh "../Mario Tennis (USA).z64"
set -e
ROM="${1:-../Mario Tennis (USA).z64}"
python tools/make_recomp_h.py
python tools/gen_symbols.py "$ROM"
python tools/check_branches.py | tail -1
python tools/find_spins.py | tail -1
rm -rf RecompiledFuncs && mkdir RecompiledFuncs
../src/N64Recomp/build/N64Recomp.exe "${MT_CONFIG:-mt.us.toml}" | grep -v "^\[Info\]" || true
../src/N64Recomp/build/RSPRecomp.exe aspMain.us.toml
# The pinned N64ModernRuntime predates SectionTableEntry::got_ram_addr; this game has no GOT.
sed -i 's/, \.got_ram_addr = std::nullopt//' RecompiledFuncs/recomp_overlays.inl
