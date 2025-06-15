#!/bin/bash

# Cross-platform setup script for MDFH
# Installs required build tools and Conan package manager

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
NC='\033[0m'

print_section() {
    echo -e "\n${BLUE}=== $1 ===${NC}"
}

print_success() {
    echo -e "${GREEN}✓ $1${NC}"
}

print_error() {
    echo -e "${RED}$1${NC}"
}

# Install packages on Debian/Ubuntu
install_linux() {
    print_section "Installing system packages (apt)"
    sudo apt-get update
    sudo apt-get install -y build-essential cmake python3 python3-pip git
    print_success "Base packages installed"

    print_section "Installing Conan via pip"
    pip3 install --user --upgrade conan
    print_success "Conan installed"
}

# Install packages on macOS using Homebrew
install_macos() {
    print_section "Installing system packages (brew)"
    if ! command -v brew &>/dev/null; then
        print_error "Homebrew not found. Please install Homebrew first: https://brew.sh/"
        exit 1
    fi
    brew update
    brew install cmake conan git
    print_success "Homebrew packages installed"
}

# Detect platform and run appropriate installer
case "$(uname)" in
    Linux)
        install_linux
        ;;
    Darwin)
        install_macos
        ;;
    *)
        print_error "Unsupported platform: $(uname)"
        exit 1
        ;;
esac

print_section "Setup complete"
print_success "You can now run ./scripts/build.sh"

