# source requrements 

libx11-dev 
pkg-config 
meson
ninja 
libxcursor-dev 
libdbus-1-dev 
libcairo-dev
xorg
cmake 

# to compile railwm

meson setup build 

ninja -C build 

# to install railwm 

copy the binaries into /usr/local/bin or if you have ~/.local/bin on PATH install it there 

## NOTE i didnt check the config patterns. once i come there i will update readme with configuration docs

to install the release binary just tar -xvf railwm-bin.tar.xz and follow the installation above


## what railwm lacks

multi monitor support 

mouse click control of the strips. only keyboard usage

no csm. but who wants it in a tiler? 

you cant move floating windows around here. but who cares. i may add controls for that. but time will tell

## future plans 

i may port this to GO language. but we will see 


## tested hardware 

3 intel pcs which are pentium which is my main pc with debian sid, 
celeron c1037 but that attempt with celeron failed because of chimera-linux weirdness,
and i3 4130 << this cpu is where railwm journey started
amd pc with fm2 socket railwm was tested on 

## tested linux distros 

chimera 
fedora 
nixos 
alpine 
void 
debian-sid 
crux 
gentoo 
venom 
opensuse

arch based distros werent tested. 
