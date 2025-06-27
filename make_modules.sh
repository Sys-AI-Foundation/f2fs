make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- olddefconfig modules -j8 &> makef2fslog.txt
cp ./fs/f2fs/f2fs.ko ../modshare/
cp ./modules.* ../modshare/