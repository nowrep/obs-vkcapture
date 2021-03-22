_pkgbase=obs-vkcapture
pkgbase=${_pkgbase}-git
pkgname=('obs-vkcapture-git' 'lib32-obs-vkcapture-git')
pkgver=0.1
pkgrel=1
url='https://github.com/nowrep/obs-vkcapture'
license=('GPL2')
arch=('x86_64')
depends=('vulkan-icd-loader' 'obs-studio-git')
makedepends=('gcc' 'cmake' 'vulkan-headers' 'lib32-vulkan-icd-loader')
source=("$_pkgbase::git+$url")
sha512sums=('SKIP')

pkgver() {
    cd "$_pkgbase"
    printf "r%s.%s" "$(git rev-list --count HEAD)" "$(git rev-parse --short HEAD)"
}

build() {
    cmake -B build -S "$_pkgbase" -DCMAKE_INSTALL_PREFIX=/usr \
        -DCMAKE_INSTALL_LIBDIR=lib \
        -DCMAKE_INSTALL_DATADIR=/share \
        -DCMAKE_BUILD_TYPE=Release
    make -C build

    export CFLAGS="-m32 ${CFLAGS}"
    export CXXFLAGS="-m32 ${CXXFLAGS}"
    export LDFLAGS="-m32 ${LDFLAGS}"

    cmake -B build32 -S "$_pkgbase" -DCMAKE_INSTALL_PREFIX=/usr \
        -DCMAKE_INSTALL_LIBDIR=lib32 \
        -DCMAKE_INSTALL_DATADIR=/share \
        -DCMAKE_LIBRARY_PATH=/usr/lib32 \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_PLUGIN=OFF
    make -C build32
}

package_obs-vkcapture-git() {
    pkgdesc='OBS Linux Vulkan game capture'
    conflicts=('obs-vkcapture')

    make -C build DESTDIR="$pkgdir" install
}

package_lib32-obs-vkcapture-git() {
    pkgdesc='OBS Linux Vulkan game capture (32-bit)'
    depends=('lib32-vulkan-icd-loader')
    conflicts=('lib32-obs-vkcapture')

    make -C build32 DESTDIR="$pkgdir" install
}
