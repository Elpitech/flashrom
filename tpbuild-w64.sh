#!/bin/sh

rm -rf libftdi libusb libusb-1.0 &&
rm -rf build &&

svn export https://team.t-platforms.ru/svn/src/vendor/libftdi/mingw32/0.18 libftdi &&
svn export https://team.t-platforms.ru/svn/src/vendor/libusb/win32-bin/1.2.4.0 libusb &&
svn export https://team.t-platforms.ru/svn/src/vendor/libusb-1.0/win32-bin/1.0.20 libusb-1.0 &&

make CC=i686-w64-mingw32-gcc \
     STRIP=i686-w64-mingw32-strip \
     AR=i686-w64-mingw32-ar \
     RANLIB=i686-w64-mingw32-ranlib \
     CPPFLAGS="-Ilibusb/include -Ilibftdi/include -Ilibusb-1.0/include/libusb-1.0" \
     LDFLAGS="-Llibusb/lib/gcc -Llibftdi/lib -Llibusb-1.0/MinGW32/dll" \
     CFLAGS="-D_GNU_SOURCE=1" &&

mkdir build &&
i686-w64-mingw32-strip flashrom.exe &&
mv flashrom.exe build/ &&
cp libftdi/dll/libftdi.dll build/ &&
cp libusb/bin/x86/libusb0_x86.dll build/libusb0.dll &&
cp libusb-1.0/MinGW32/dll/libusb-1.0.dll build/ &&

make clean &&

make CC="gcc -m32" &&
strip flashrom &&
mv flashrom build/flashrom-centos6.2.i686

