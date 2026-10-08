source requrements 

libx11-dev 
pkg-config 
meson
ninja 
libxcursor-dev 
libdbus-1-dev 
libcairo-dev
xorg
cmake 

to compile this 

meson setup build 
ninja -C build 

to install this 

copy the binaries into /usr/local/bin or if you have ~/.local/bin on PATH install it there 
NOTE i didnt check the config patterns. once i come there i will update readme with configuration docs

to install the release binary just tar -xvf railwm-bin.tar.xz and follow the installation above
