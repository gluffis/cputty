#!/bin/bash
#
# Build cputty release artifacts:
#
#   cputty-VERSION-linux-x86_64.tar.gz    Linux binaries (dynamically
#                                         linked; built on this system)
#   cputty_VERSION_amd64.deb              cputty only; coexists with
#                                         Debian's putty packages
#   cputty-suite_VERSION_amd64.deb        SSH-only putty, plink, pscp,
#                                         psftp, puttygen, pageant;
#                                         replaces Debian's putty and
#                                         putty-tools
#   cputty-VERSION-windows-x86_64.zip     Windows 64-bit .exe files
#   cputty-VERSION-windows-x86_64.msi     the same, as an installer
#   SHA256SUMS
#
# Usage: release/build-release.sh VERSION [OUTPUT_DIR]
#
# Needs: cmake, gcc, GTK 3 development files, mingw-w64, halibut (for
# the man pages), dpkg-dev (dpkg-deb, dpkg-shlibdeps), fakeroot, zip,
# msitools (wixl, for the .msi). VERSION must match the RELEASE defined
# in version.h.

set -euo pipefail

VERSION=${1:?usage: $0 VERSION [OUTPUT_DIR]}
SRC=$(cd "$(dirname "$0")/.." && pwd)
OUT=${2:-$SRC/release-out}
JOBS=$(nproc)
MAINTAINER="$(git -C "$SRC" config user.name) <$(git -C "$SRC" config user.email)>"
HOMEPAGE="https://github.com/gluffis/cputty"

# Refuse to label a build with a version it wasn't compiled with
if ! tr -d '\r' < "$SRC/version.h" | grep -qx "#define RELEASE $VERSION"; then
    echo "error: version.h does not define RELEASE $VERSION" >&2
    exit 1
fi

for tool in halibut dpkg-deb dpkg-shlibdeps fakeroot zip wixl \
            x86_64-w64-mingw32-strip; do
    if ! command -v "$tool" > /dev/null; then
        echo "error: $tool not found" >&2
        exit 1
    fi
done

TOOLS="putty plink pscp psftp puttygen pageant"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$OUT"

echo "== Building Linux binaries"
cmake -S "$SRC" -B "$work/linux" -DCMAKE_BUILD_TYPE=Release > "$work/linux.log"
cmake --build "$work/linux" -j"$JOBS" --target cputty $TOOLS manpages \
      >> "$work/linux.log"

echo "== Building Windows binaries"
cmake -S "$SRC" -B "$work/win64" \
      -DCMAKE_TOOLCHAIN_FILE="$SRC/cmake/toolchain-mingw.cmake" > "$work/win64.log"
cmake --build "$work/win64" -j"$JOBS" --target cputty $TOOLS >> "$work/win64.log"

# ----------------------------------------------------------------------
echo "== Linux tarball"
tarname="cputty-$VERSION-linux-x86_64"
mkdir -p "$work/$tarname"
for b in cputty $TOOLS; do
    install -m 755 "$work/linux/$b" "$work/$tarname/$b"
    strip "$work/$tarname/$b"
    install -D -m 644 "$work/linux/doc/$b.1" "$work/$tarname/man/man1/$b.1"
done
install -m 644 "$SRC/LICENCE" "$work/$tarname/LICENCE"
cat > "$work/$tarname/README.txt" <<EOF
cputty $VERSION for Linux x86_64

cputty is a cluster-SSH tool based on PuTTY 0.85: it opens one PuTTY
terminal window per host plus a console window whose keystrokes are
sent to every ticked session.

  ./cputty -l user host1 host2 host3
  man -l man/man1/cputty.1

Also included: SSH-only builds of putty, plink, pscp, psftp, puttygen
and pageant (Telnet, Rlogin, SUPDUP, Raw and serial support removed).

These binaries are dynamically linked against GTK 3 and glibc and
were built on Debian 13; they need an equally recent distribution.

Source: $HOMEPAGE
Licence: MIT (see LICENCE). This is a modified version of PuTTY and is
not supported by the PuTTY developers.
EOF
chmod 644 "$work/$tarname/README.txt"
find "$work/$tarname" -type d -exec chmod 755 {} +
tar -C "$work" --owner=0 --group=0 -czf "$OUT/$tarname.tar.gz" "$tarname"

# ----------------------------------------------------------------------
# make_deb PACKAGE "SHORT DESCRIPTION" "LONG DESCRIPTION" EXTRA_FIELDS BINARIES...
make_deb() {
    local pkg=$1 short=$2 long=$3 extra=$4
    shift 4
    local root="$work/deb-$pkg/root"
    mkdir -p "$root/DEBIAN" "$root/usr/bin" "$root/usr/share/doc/$pkg" \
             "$root/usr/share/man/man1"

    for b in "$@"; do
        install -m 755 "$work/linux/$b" "$root/usr/bin/$b"
        strip "$root/usr/bin/$b"
        gzip -9n < "$work/linux/doc/$b.1" > "$root/usr/share/man/man1/$b.1.gz"
        chmod 644 "$root/usr/share/man/man1/$b.1.gz"
    done

    install -m 644 "$SRC/LICENCE" "$root/usr/share/doc/$pkg/copyright"
    printf '%s (%s) unstable; urgency=medium\n\n  * Release %s.\n\n -- %s  %s\n' \
        "$pkg" "$VERSION" "$VERSION" "$MAINTAINER" "$(date -R)" \
        | gzip -9n > "$root/usr/share/doc/$pkg/changelog.Debian.gz"
    chmod 644 "$root/usr/share/doc/$pkg/changelog.Debian.gz"

    # Work out shared-library dependencies (dpkg-shlibdeps wants a
    # debian/control to exist in the current directory)
    mkdir -p "$work/deb-$pkg/debian"
    printf 'Source: %s\n\nPackage: %s\nArchitecture: amd64\n' "$pkg" "$pkg" \
        > "$work/deb-$pkg/debian/control"
    local shlibs
    shlibs=$(cd "$work/deb-$pkg" &&
             dpkg-shlibdeps -O $(printf 'root/usr/bin/%s ' "$@") 2>/dev/null \
             | sed -n 's/^shlibs:Depends=//p')

    local size
    size=$(du -sk --exclude=DEBIAN "$root" | cut -f1)
    {
        echo "Package: $pkg"
        echo "Version: $VERSION"
        echo "Architecture: amd64"
        echo "Maintainer: $MAINTAINER"
        echo "Installed-Size: $size"
        echo "Depends: $shlibs"
        [ -n "$extra" ] && printf '%s\n' "$extra"
        echo "Section: net"
        echo "Priority: optional"
        echo "Homepage: $HOMEPAGE"
        echo "Description: $short"
        printf '%s\n' "$long" | sed 's/^$/./; s/^/ /'
    } > "$root/DEBIAN/control"

    # Debian expects 755 directories, whatever the builder's umask is
    find "$root" -type d -exec chmod 755 {} +
    fakeroot dpkg-deb --build --root-owner-group "$root" \
        "$OUT/${pkg}_${VERSION}_amd64.deb" > /dev/null
}

echo "== Debian packages"
make_deb cputty \
    "cluster SSH client: one console typing into many PuTTY sessions" \
    "cputty opens one PuTTY terminal window per host given on the command
line, plus a console window. Keystrokes typed into the console are sent
to every session ticked in the console's host list, in the style of the
classic cssh tool. Typing into a terminal window affects only that
session.

This package installs only cputty and can be installed alongside
Debian's putty packages." \
    "" \
    cputty

make_deb cputty-suite \
    "SSH-only PuTTY tools from the cputty project" \
    "SSH-only builds of PuTTY (putty, plink, pscp, psftp, puttygen,
pageant) from the cputty project, based on PuTTY 0.85. Telnet, Rlogin,
SUPDUP, Raw and serial-line support have been removed.

This package replaces Debian's putty and putty-tools packages." \
    "Conflicts: putty, putty-tools
Replaces: putty, putty-tools" \
    $TOOLS

# ----------------------------------------------------------------------
echo "== Windows zip"
zipname="cputty-$VERSION-windows-x86_64"
mkdir -p "$work/$zipname"
chmod 755 "$work/$zipname"
for b in cputty $TOOLS; do
    install -m 644 "$work/win64/$b.exe" "$work/$zipname/$b.exe"
    x86_64-w64-mingw32-strip "$work/$zipname/$b.exe"
done
sed 's/$/\r/' "$SRC/LICENCE" > "$work/$zipname/LICENCE.txt"
sed 's/$/\r/' > "$work/$zipname/README.txt" <<EOF
cputty $VERSION for Windows x86_64

cputty is a cluster-SSH tool based on PuTTY 0.85: it opens one PuTTY
terminal window per host plus a console window whose keystrokes are
sent to every ticked session.

  cputty.exe -l user host1 host2 host3

Also included: SSH-only builds of putty, plink, pscp, psftp, puttygen
and pageant (Telnet, Rlogin, SUPDUP, Raw and serial support removed).

The executables are not code-signed, so Windows may warn when they are
first run.

Source: $HOMEPAGE
Licence: MIT (see LICENCE.txt). This is a modified version of PuTTY and
is not supported by the PuTTY developers.
EOF
chmod 644 "$work/$zipname/LICENCE.txt" "$work/$zipname/README.txt"
rm -f "$OUT/$zipname.zip"
(cd "$work" && zip -qr -X "$OUT/$zipname.zip" "$zipname")

# ----------------------------------------------------------------------
echo "== Windows installer"
# Built from the files just staged for the zip, so both contain the
# same stripped executables
rm -f "$OUT/$zipname.msi"
wixl -a x64 -D Version="$VERSION" -D Builddir="$work/$zipname" \
     -D Sourcedir="$SRC" -o "$OUT/$zipname.msi" \
     "$SRC/windows/cputty-installer.wxs"

# ----------------------------------------------------------------------
(cd "$OUT" && sha256sum "$tarname.tar.gz" "cputty_${VERSION}_amd64.deb" \
     "cputty-suite_${VERSION}_amd64.deb" "$zipname.zip" "$zipname.msi" \
     > SHA256SUMS)

echo "== Done: artifacts in $OUT"
ls -l "$OUT"
