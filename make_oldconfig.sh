# make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- olddefconfig Image modules -j8 &> makelog.txt
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- olddefconfig Image -j8 &> makelog.txt
# cp ./fs/f2fs/f2fs.ko ../modshare/
# cp ./modules.* ../modshare/
# cp ./lib/zstd/zstd_compress.ko ../modshare/
# cp ./lib/lz4/lz4_compress.ko ../modshare/
# cp ./lib/lz4/lz4hc_compress.ko ../modshare/