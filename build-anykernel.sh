#!/bin/bash

###########################################################################################
# NebulaKernel Build Script (C) 2015                                                      #
#                                                                                         #
#                                                                                         #
# Build Script W/AnyKernel V2 Support Plus        07/22/2015                              #
#                                                                                         #
# Added: Random+YYYYMMDD Format at end of zip                                             #
# Added: SignApk to sign all zips                                                         #
# Added Build.log Error Only or Full Log                                                  #
#                                                                                         #
#                                                                                         #
#                                                                                         #
#                                                                                         #
#                                                                                         #
#                                                                                         #
#                                                                                         #
#                                                                                         #
#                                                                                         #
#                                                                                         #
#                                                                                         #
###########################################################################################

# Bash Color
green='\033[01;32m'
red='\033[01;31m'
blink_red='\033[05;31m'
restore='\033[0m'

clear

# Resources
THREAD="-j$(grep -c ^processor /proc/cpuinfo)"
KERNEL="zImage"
DTBIMAGE="dtb"

# Kernel Details
VER=NebulaKernel
REV="Rev6.5"
#BDATE=$(date +"%Y%m%d")
KVER="$RANDOM"_$(date +"%Y%m%d")


# Vars
export LOCALVERSION=~`echo $VER`
export CROSS_COMPILE=${HOME}/Builds/KERNEL-SOURCE/toolchains/arm-eabi-6.0/bin/arm-eabi-
export ARCH=arm
export SUBARCH=arm
export KBUILD_BUILD_USER=Eliminater74
export KBUILD_BUILD_HOST=HP_ENVY_dv7.com
export CCACHE=ccache

# Paths
KERNEL_DIR=`pwd`
REPACK_DIR="${HOME}/Builds/KERNEL-SOURCE/G3-AnyKernel"
PATCH_DIR="${HOME}/Builds/KERNEL-SOURCE/G3-AnyKernel/patch"
MODULES_DIR="${HOME}/Builds/KERNEL-SOURCE/G3-AnyKernel/modules"
TOOLS_DIR="${HOME}/Builds/KERNEL-SOURCE/G3-AnyKernel/tools"
RAMDISK_DIR="${HOME}/Builds/KERNEL-SOURCE/G3-AnyKernel/ramdisk"
SIGNAPK="${HOME}/Builds/KERNEL-SOURCE/SignApk/signapk.jar"
SIGNAPK_KEYS="${HOME}/Builds/KERNEL-SOURCE/SignApk"
ZIP_MOVE="${HOME}/Builds/KERNEL-SOURCE/zips"
ZIMAGE_DIR="${HOME}/Builds/KERNEL-SOURCE/NebulaKernel/arch/arm/boot"

# Functions
function clean_all {
		rm -rf $MODULES_DIR/*
		#rm -rf ~/.ccache
		cd $REPACK_DIR
		rm -rf $KERNEL
		rm -rf $DTBIMAGE
		rm -rf *.zip
		cd $KERNEL_DIR
		echo "Deleting arch/arm/boot/*.dtb's"
		rm -rf arch/arm/boot/*dtb
		echo "Deleting arch/arm/boot/zImage*"
		rm -rf arch/arm/boot/zImage*
		echo "Deleting arch/arm/boot/Image*"
		rm -rf arch/arm/boot/Image*
		echo "Deleting firmware/synaptics/g3/*.gen.*"
		rm -rf firmware/synaptics/g3/*gen*
		echo
		make clean && make mrproper
}

function change_variant {
		cd $REPACK_DIR
        sed -i 's/d850/$VARIANT/g; s/d851/$VARIANT/g; s/d852/$VARIANT/g; s/d855/$VARIANT/g; s/f400/$VARIANT/g; s/ls990/$VARIANT/g; s/vs985/$VARIANT/g/g' anykernel.sh
		UP_CASE=$VARIANT | tr '[:upper:]' '[:lower:]'
		sed -i 's/D850/$VARIANT/g; s/D851/$VARIANT/g; s/D852/$VARIANT/g; s/D855/$VARIANT/g; s/F400/$VARIANT/g; s/LS990/$VARIANT/g; s/VS985/$VARIANT/g/g' anykernel.sh
		cd $KERNEL_DIR
}

function build_log {
		rm -rf build.log
        exec 2> >(sed -r 's/'$(echo -e "\033")'\[[0-9]{1,2}(;([0-9]{1,2})?)?[mK]//g' | tee -a build.log)
}

function make_kernel {
		echo
		make $DEFCONFIG
		make $THREAD
		cp -vr $ZIMAGE_DIR/$KERNEL $REPACK_DIR
}

function make_modules {
		rm `echo $MODULES_DIR"/*"`
		find $KERNEL_DIR -name '*.ko' -exec cp -v {} $MODULES_DIR \;
}

function make_dtb {
		$REPACK_DIR/tools/dtbToolCM -2 -o $REPACK_DIR/$DTBIMAGE -s 2048 -p scripts/dtc/ arch/arm/boot/
}

function make_zip {
		cd $REPACK_DIR
		zip -r9 NebulaKernel_"$REV"_MR_"$VARIANT"_"$KVER".zip *
		#java -jar $SIGNAPK $SIGNAPK_KEYS/testkey.x509.pem $SIGNAPK_KEYS/testkey.pk8 NebulaKernel_"$REV"_MR_"$VARIANT"_"$KVER".zip NebulaKernel_"$REV"_MR_"$VARIANT"_"$KVER"-signed.zip
		#mv NebulaKernel_"$REV"_MR_"$VARIANT"_"$KVER"-signed.zip $ZIP_MOVE
		mv NebulaKernel_"$REV"_MR_"$VARIANT"_"$KVER".zip $ZIP_MOVE
		rm -rf NebulaKernel_"$REV"_MR_"$VARIANT"_"$KVER".zip
		cd $KERNEL_DIR
}


DATE_START=$(date +"%s")

echo -e "${green}"
echo "NebulaKerrnel Creation Script:"
echo -e "${restore}"

echo "Pick LG G3 VARIANT..."
select choice in d850 d851 d852 d855 d855-low f400 ls990 vs985
do
case "$choice" in
	"d850")
		VARIANT="d850"
		DEFCONFIG="d850_defconfig"
		break;;
	"d851")
		VARIANT="d851"
		DEFCONFIG="d851_defconfig"
		break;;
	"d852")
		VARIANT="d852"
		DEFCONFIG="d852_defconfig"
		break;;
	"d855")
		VARIANT="d855"
		DEFCONFIG="d855_defconfig"
		break;;
	"d855-low")
		VARIANT="d855-low"
		DEFCONFIG="d855_lowmem_defconfig"
		break;;
	"ls990")
		VARIANT="ls990"
		DEFCONFIG="ls990_defconfig"
		break;;
	"vs985")
		VARIANT="vs985"
		DEFCONFIG="vs985_defconfig"
		break;;
	"f400")
		VARIANT="f400"
		DEFCONFIG="f400_defconfig"
		break;;	
esac
done

while read -p "Do you want to Make clean,mrproper and clear ccache (Y/n)? " cchoice
do
case "$cchoice" in
	y|Y )
		clean_all
		echo
		echo "All Cleaned now."
		break
		;;
	n|N )
		break
		;;
	* )
		echo
		echo "Invalid try again!"
		echo
		;;
esac
done

echo

while read -p "Are you sure you want to build the kernel (y/n)? " dchoice
do
case "$dchoice" in
	y|Y)
		#build_log
		make_kernel
		make_dtb
		make_modules
		make_zip
		break
		;;
	n|N )
		break
		;;
	* )
		echo
		echo "Invalid try again!"
		echo
		;;
esac
done

echo -e "${green}"
echo "--------------------------------------------------------"
echo "NebulaKernel_'$REV'_MR_'$VARIANT'_'$KVER'-signed.zip"
echo "Created Successfully.."
echo "Build Completed in:"
echo "--------------------------------------------------------"
echo -e "${restore}"

DATE_END=$(date +"%s")
DIFF=$(($DATE_END - $DATE_START))
echo "Time: $(($DIFF / 60)) minute(s) and $(($DIFF % 60)) seconds."
echo
