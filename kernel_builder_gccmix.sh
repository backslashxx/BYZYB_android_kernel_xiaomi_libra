#!/bin/bash


GCC64="/tmp/optane/gcc/android_prebuilts_gcc_linux-x86_aarch64_aarch64-linux-android-4.9-lineage-19.1"
GCC32="/tmp/optane/gcc/android_prebuilts_gcc_linux-x86_arm_arm-linux-androideabi-4.9-lineage-19.1"
export PATH="$PREFIX$GCC64/bin:$PREFIX$GCC32/bin:$PATH"

export CROSS_COMPILE=aarch64-linux-android-
export ARCH=arm64
export SUBARCH=arm64
export HEADER_ARCH=arm64

#rm -rf out
mkdir out
#rm -rf error.log
#make O=out clean 
#make mrproper

export CROSS_COMPILE=aarch64-linux-android-
echo $PATH

ARCH=arm64 scripts/kconfig/merge_config.sh -O out arch/arm64/configs/libra_defconfig libra_defconfig_append_gcc > /dev/null 2>&1 


make -j24 ARCH=arm64 O=out SUBARCH=arm64 O=out \
	CC="ccache aarch64-linux-android-gcc" \
        LD="aarch64-linux-android-ld.bfd" \
        AR="aarch64-linux-android-ar" \
        AS="aarch64-linux-android-as" \
        NM="aarch64-linux-android-nm" \
        CROSS_COMPILE_ARM32="ccache arm-linux-androideabi-" \
        OBJCOPY="aarch64-linux-android-objcopy" \
        OBJDUMP="aarch64-linux-android-objdump" \
        STRIP="aarch64-linux-android-strip" \
        CROSS_COMPILE="ccache aarch64-linux-android-" \
	KBUILD_BUILD_USER="$(git rev-parse --short HEAD | cut -c1-7)" \
	KBUILD_BUILD_HOST="$(git symbolic-ref --short HEAD)" \
#echo $LD

# for i in $(ls patches_los16/) ; do patch -Np1 < patches_los16/$i ; done
