makedir:
    mkdir -p build

[working-directory: 'build']
build: makedir
    cmake -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_BUILD_TYPE=Release ..
    make
    sudo make install
