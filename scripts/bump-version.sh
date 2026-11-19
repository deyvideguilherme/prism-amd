#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# bump-version.sh - Synchronize and validate version across all Prisma Control Center components.

set -euo pipefail

if [ "$#" -ne 1 ]; then
    echo "Usage: $0 <version>" >&2
    echo "Example: $0 1.0.0" >&2
    exit 1
fi

NEW_VER="$1"

# Validate semver format (X.Y.Z)
if [[ ! "$NEW_VER" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    echo "Error: Invalid version format '$NEW_VER'. Expected semantic versioning (e.g. 1.0.0)." >&2
    exit 1
fi

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

echo "Synchronizing project files to version ${NEW_VER}..."

# 1. CMakeLists.txt
sed -i -E 's/(^[[:space:]]*VERSION[[:space:]]+)[0-9]+\.[0-9]+\.[0-9]+/\1'"$NEW_VER"'/g' "$REPO_ROOT/CMakeLists.txt"

# 2. kernel/dkms.conf
sed -i -E 's/PACKAGE_VERSION="[0-9]+\.[0-9]+\.[0-9]+"/PACKAGE_VERSION="'"$NEW_VER"'"/g' "$REPO_ROOT/kernel/dkms.conf"

# 3. kernel/prisma_abi.h
sed -i -E 's/#define PRISMA_VERSION "[0-9]+\.[0-9]+\.[0-9]+"$/#define PRISMA_VERSION "'"$NEW_VER"'"/g' "$REPO_ROOT/kernel/prisma_abi.h"

# 4. packaging/arch/PKGBUILD
sed -i -E 's/^pkgver=[0-9]+\.[0-9]+\.[0-9]+/pkgver='"$NEW_VER"'/g' "$REPO_ROOT/packaging/arch/PKGBUILD"
sed -i -E 's/^pkgrel=[0-9]+/pkgrel=1/g' "$REPO_ROOT/packaging/arch/PKGBUILD"

# 5. ui/prisma/plugin.toml
sed -i -E 's/^version = "[0-9]+\.[0-9]+\.[0-9]+"/version = "'"$NEW_VER"'"/g' "$REPO_ROOT/ui/prisma/plugin.toml"

# 6. Regenerate packaging/arch/.SRCINFO via makepkg
if command -v makepkg >/dev/null 2>&1; then
    (
        cd "$REPO_ROOT/packaging/arch"
        makepkg --printsrcinfo > .SRCINFO
    )
else
    echo "Warning: makepkg not found in PATH; updating .SRCINFO directly." >&2
    sed -i -E 's/pkgver = [0-9]+\.[0-9]+\.[0-9]+/pkgver = '"$NEW_VER"'/g' "$REPO_ROOT/packaging/arch/.SRCINFO"
    sed -i -E 's/pkgrel = [0-9]+/pkgrel = 1/g' "$REPO_ROOT/packaging/arch/.SRCINFO"
fi

# 7. Verification: Ensure no diverging version remains
ERRORS=0

verify_file() {
    local file="$1"
    local pattern="$2"
    local desc="$3"

    if ! grep -qE "$pattern" "$file"; then
        echo "Error: Version mismatch in $desc ($file)" >&2
        ERRORS=$((ERRORS + 1))
    fi
}

verify_file "$REPO_ROOT/CMakeLists.txt" "^[[:space:]]*VERSION[[:space:]]+$NEW_VER" "CMakeLists.txt"
verify_file "$REPO_ROOT/kernel/dkms.conf" "PACKAGE_VERSION=\"$NEW_VER\"" "kernel/dkms.conf"
verify_file "$REPO_ROOT/kernel/prisma_abi.h" "#define PRISMA_VERSION \"$NEW_VER\"" "kernel/prisma_abi.h"
verify_file "$REPO_ROOT/packaging/arch/PKGBUILD" "^pkgver=$NEW_VER" "packaging/arch/PKGBUILD"
verify_file "$REPO_ROOT/packaging/arch/.SRCINFO" "pkgver = $NEW_VER" "packaging/arch/.SRCINFO"
verify_file "$REPO_ROOT/ui/prisma/plugin.toml" "^version = \"$NEW_VER\"" "ui/prisma/plugin.toml"

if [ "$ERRORS" -gt 0 ]; then
    echo "Validation failed: $ERRORS files have diverging versions." >&2
    exit 1
fi

echo "All version files successfully synchronized and verified: ${NEW_VER}"
