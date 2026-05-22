#!/usr/bin/env bash
# install_deps.sh - Install Debian build dependencies for the native Linux
# build of the Burnout 3 static recompilation.
#
# Run from WSL Debian (or any Debian/Ubuntu host):
#   bash tools/linux/install_deps.sh
#
# Requires sudo (will prompt for a password).
set -e

PKGS=(
    cmake               # build system
    build-essential     # gcc / make / libc headers
    pkg-config          # dependency discovery
    libsdl2-dev         # window, input, audio  (Stages 2-3)
    libgl1-mesa-dev     # OpenGL headers        (Stage 4)
    libepoxy-dev        # OpenGL function loader (Stage 4)
    libssl-dev          # crypto, replaces bcrypt (Stage 1)
    libavcodec-dev      # video decode          (Stage 5)
    libavformat-dev     # video demux           (Stage 5)
    libavutil-dev       # video support         (Stage 5)
    libswscale-dev      # video colour convert  (Stage 5)
)

echo "Installing ${#PKGS[@]} packages: ${PKGS[*]}"
sudo apt-get update
sudo apt-get install -y "${PKGS[@]}"
echo
echo "Done. Build deps installed."
