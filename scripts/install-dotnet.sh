#!/usr/bin/env bash
# Install the .NET 8 LTS SDK into deps/dotnet/ for the betl-dotnet
# provider (v0.2+ feature: dotnet.task / dotnet.script).
#
# License: MIT. See docs/THIRD_PARTY_LICENSES.md for the audit. The
# pinned SDK version + SHA256 in this script must agree with the
# audit doc — re-run the audit when bumping either.
#
# Why a project-local script rather than install_dep_source: .NET
# is a pre-built binary tarball, not a buildable source tree, and
# living in betl/scripts keeps the .NET dependency in the project
# structure (consistent with the libyaml / libpq pattern) rather
# than baked into the c-build container.
#
# Idempotent: if deps/dotnet/dotnet exists and matches the pinned
# version, exit fast. Pass --force to reinstall.

set -euo pipefail

# --- Pinned versions (must match docs/THIRD_PARTY_LICENSES.md) ---
DOTNET_VERSION="8.0.404"
DOTNET_RUNTIME_VERSION="8.0.11"   # informational; the SDK bundles it
DOTNET_URL="https://dotnetcli.azureedge.net/dotnet/Sdk/${DOTNET_VERSION}/dotnet-sdk-${DOTNET_VERSION}-linux-x64.tar.gz"
DOTNET_SHA256="5bf340ba6acb314c703c2492a3a6e1530d7cdbfd3b5bf86788ee8b4afefd3573"

# --- Paths ---
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
DEPS_DIR="$PROJECT_ROOT/deps/dotnet"

force=0
for arg in "$@"; do
    case "$arg" in
        --force) force=1 ;;
        -h|--help)
            echo "Usage: $0 [--force]"
            echo
            echo "Installs .NET ${DOTNET_VERSION} SDK into $DEPS_DIR"
            echo "License: MIT (see docs/THIRD_PARTY_LICENSES.md)"
            exit 0
            ;;
        *) echo "unknown arg: $arg" >&2; exit 2 ;;
    esac
done

# --- Fast-path: already installed at the right version ---
if [[ $force -eq 0 && -x "$DEPS_DIR/dotnet" ]]; then
    if installed=$("$DEPS_DIR/dotnet" --version 2>/dev/null); then
        if [[ "$installed" == "$DOTNET_VERSION" ]]; then
            echo "deps/dotnet: $DOTNET_VERSION already installed"
            exit 0
        fi
        echo "deps/dotnet: replacing $installed → $DOTNET_VERSION"
    fi
fi

mkdir -p "$DEPS_DIR"
tmpdir="$(mktemp -d)"
trap 'rm -rf "$tmpdir"' EXIT

tarball="$tmpdir/dotnet-sdk.tar.gz"
echo "Downloading $DOTNET_URL"
curl -fL --progress-bar -o "$tarball" "$DOTNET_URL"

actual_sha=$(sha256sum "$tarball" | awk '{print $1}')
if [[ "$actual_sha" != "$DOTNET_SHA256" ]]; then
    echo "SHA256 mismatch!"  >&2
    echo "  expected: $DOTNET_SHA256" >&2
    echo "  actual:   $actual_sha"    >&2
    exit 1
fi
echo "SHA256 OK"

# Wipe any prior install. Keep the directory itself in case something
# was symlinked at the parent (deps/ is shared by all installed deps).
if [[ -d "$DEPS_DIR" && $force -eq 1 ]]; then
    rm -rf "$DEPS_DIR"
    mkdir -p "$DEPS_DIR"
fi

echo "Extracting to $DEPS_DIR"
tar -xzf "$tarball" -C "$DEPS_DIR"

# Sanity check: the tarball ships LICENSE.txt + ThirdPartyNotices.txt
# at the top level. Confirm they're there — they're the attribution
# the MIT license requires us to redistribute alongside the binaries.
for f in LICENSE.txt ThirdPartyNotices.txt; do
    if [[ ! -f "$DEPS_DIR/$f" ]]; then
        echo "warning: $f missing from extracted SDK" >&2
    fi
done

# Check NativeAOT host-toolchain prereqs. NativeAOT uses clang as the
# linker driver and links against zlib at static-link time, so a host
# missing either of them produces a confusing "cannot find -lz" or
# "clang not found" failure deep inside `dotnet publish`. We don't
# install these from this script (they're host-pkg-manager territory
# alongside gcc itself) — just flag the gap clearly.
echo
echo "--- NativeAOT host prerequisites ---"
missing=0
if ! command -v clang >/dev/null 2>&1; then
    echo "  ✗ clang not on PATH"
    echo "    Install with: sudo apt-get install -y clang"
    missing=1
else
    echo "  ✓ clang ($(clang --version | head -1))"
fi
if ! ldconfig -p 2>/dev/null | grep -q "libz\.so"; then
    echo "  ✗ libz not on link path"
    echo "    Install with: sudo apt-get install -y zlib1g-dev"
    missing=1
else
    echo "  ✓ libz on link path"
fi
if [[ $missing -ne 0 ]]; then
    echo
    echo "Install the missing packages above before running dotnet.task /"
    echo "dotnet.script. AOT compilation will fail at link time without them."
    echo
fi

# Verify the dotnet CLI actually runs. Globalization-invariant mode
# avoids a hard libicu dependency at build time — the AOT compiler
# itself doesn't need locale-aware string handling. Production-time
# user code that genuinely depends on culture-aware comparisons can
# disable invariant mode in its own csproj.
export DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1
"$DEPS_DIR/dotnet" --version
echo "deps/dotnet: install complete"
echo
echo "To use the SDK from this betl checkout:"
echo "  export DOTNET_ROOT=\"$DEPS_DIR\""
echo "  export PATH=\"$DEPS_DIR:\$PATH\""
echo "  export DOTNET_SYSTEM_GLOBALIZATION_INVARIANT=1   # if no libicu installed"
