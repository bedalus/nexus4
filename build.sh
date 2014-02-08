    echo -e "Making mako zImage\n"
    export PATH=$PATH:/opt/toolchain3/bin/
    export ARCH=arm
    export SUBARCH=arm
    export CROSS_COMPILE=arm-cortex_a15-linux-gnueabihf-

git apply -R cm.patch
make -j7

# copy zImage
cp -f arch/arm/boot/zImage ../zip/kernel/
ls -l ../zip/kernel/zImage
cd ../zip
zip -r -9 kernel.zip * > /dev/null
mv kernel.zip ../

echo "Press enter to build cm kernel"
read enterkey
cd ../nexus4
git apply cm.patch
make -j7

# copy zImage
cp -f arch/arm/boot/zImage ../zip/kernel/
ls -l ../zip/kernel/zImage
cd ../zip
zip -r -9 kernel_cm.zip * > /dev/null
mv kernel_cm.zip ../
cd ../nexus4
git apply -R cm.patch

