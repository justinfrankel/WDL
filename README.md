To compile

cd swell
make clean && make PRELOAD_GDK=1 WAYLAND=1
sudo cp libSwell.so /usr/lib/REAPER/libSwell.so
