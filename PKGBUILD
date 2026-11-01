# Maintainer: WhitePeace36 <timon2201@gmail.com>
#
# Builds the package from this source tree; nothing is downloaded.
#
#   makepkg -si      build + install
#   makepkg -sif     rebuild + reinstall after changing the code
#
# Build files go to build/, the finished package lands next to this PKGBUILD.

# makepkg's default work dir would be ./src, which is where the source code
# lives (and `makepkg -c` / `-C` delete it). Redirect it to ./build unless the
# user configured their own BUILDDIR in makepkg.conf.
if [[ -n ${startdir:-} && ${BUILDDIR:-$startdir} -ef $startdir ]]; then
    BUILDDIR="$startdir/build"
fi

pkgname=ananicy-configurator
pkgver=1.0.r2.5d3b73e
pkgrel=1
pkgdesc="Qt GUI to edit ananicy / ananicy-cpp types, rules and ananicy.conf"
arch=('x86_64' 'aarch64')
url="https://github.com/WhitePeace36/AnanicyConfigurator"
license=('GPL-2.0-only')
depends=('qt6-base' 'polkit' 'systemd' 'hicolor-icon-theme' 'gcc-libs' 'glibc')
makedepends=('cmake')
optdepends=('ananicy-cpp: the daemon this program configures'
            'ananicy: original ananicy daemon (alternative to ananicy-cpp)')

pkgver() {
    local ver
    ver=$(sed -n 's/^project([^)]*VERSION \([0-9.]*\).*/\1/p' "$startdir/CMakeLists.txt")
    if git -C "$startdir" rev-parse --git-dir >/dev/null 2>&1; then
        printf "%s.r%s.%s" "$ver" "$(git -C "$startdir" rev-list --count HEAD)" \
            "$(git -C "$startdir" rev-parse --short=7 HEAD)"
    else
        printf "%s" "$ver"
    fi
}

build() {
    cmake -B build -S "$startdir" \
        -DCMAKE_BUILD_TYPE=None \
        -DCMAKE_INSTALL_PREFIX=/usr \
        -Wno-dev
    cmake --build build
}

package() {
    DESTDIR="$pkgdir" cmake --install build
    install -Dm644 "$startdir/LICENSE" "$pkgdir/usr/share/licenses/$pkgname/LICENSE"
    install -Dm644 "$startdir/README.md" "$pkgdir/usr/share/doc/$pkgname/README.md"
}
