    echo -e "Making mako zImage\n"
    export PATH=$PATH:/opt/toolchain3/bin/
    export ARCH=arm
    export SUBARCH=arm
    export CROSS_COMPILE=arm-cortex_a15-linux-gnueabihf-

echo "Building cm kernel"
git apply cm.patch
make -j7

# copy zImage
cp -f arch/arm/boot/zImage ../zip/kernel/
ls -l ../zip/kernel/zImage
cd ../zip
zip -r -9 moob_vNcm.zip * > /dev/null
mv moob_vNcm.zip ../

echo "Press enter to build AOSP kernel"
read enterkey
cd ../nexus4
git apply -R cm.patch
make -j7

# copy zImage
cp -f arch/arm/boot/zImage ../zip/kernel/
ls -l ../zip/kernel/zImage
cd ../zip
zip -r -9 moob_vN.zip * > /dev/null
mv moob_vN.zip ../
cd ../nexus4
git apply cm.patch

