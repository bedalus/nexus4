    echo -e "Making mako zImage\n"
    export PATH=$PATH:/opt/toolchain3/bin/
    export ARCH=arm
    export SUBARCH=arm
    export CROSS_COMPILE=arm-unknown-linux-gnueabi-

make -j7

# copy zImage
cp -f arch/arm/boot/zImage ../zip/kernel/
ls -l ../zip/kernel/zImage
cd ../zip
zip -r -9 kernel.zip * > /dev/null
mv kernel.zip ../../Documents_OSX/

