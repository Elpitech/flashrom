#!/bin/sh

rm -rf libftdi libusb libusb-1.0 &&
rm -rf build &&

svn export https://team.t-platforms.ru/svn/src/vendor/libftdi/mingw32/0.18 libftdi &&
svn export https://team.t-platforms.ru/svn/src/vendor/libusb/win32-bin/1.2.4.0 libusb &&
svn export https://team.t-platforms.ru/svn/src/vendor/libusb-1.0/win32-bin/1.0.20 libusb-1.0 &&

make CC=i586-mingw32msvc-gcc \
     STRIP=i586-mingw32msvc-strip \
     AR=i586-mingw32msvc-ar \
     RANLIB=i586-mingw32msvc-ranlib \
     CPPFLAGS="-Ilibusb/include -Ilibftdi/include -Ilibusb-1.0/include/libusb-1.0" \
     LDFLAGS="-Llibusb/lib/gcc -Llibftdi/lib -Llibusb-1.0/MinGW32/dll" \
     CFLAGS="-D_GNU_SOURCE=1" &&

mkdir build &&
i586-mingw32msvc-strip flashrom.exe &&
mv flashrom.exe build/ &&
cp libftdi/dll/libftdi.dll build/ &&
cp libusb/bin/x86/libusb0_x86.dll build/libusb0.dll &&
cp libusb-1.0/MinGW32/dll/libusb-1.0.dll build/ &&

make clean &&

make CC="gcc -m32" &&
strip flashrom &&
mv flashrom build/flashrom-centos6.2.i686

