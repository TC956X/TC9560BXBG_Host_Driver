TC9560 HSIC Driver 
Release Date: 05 April 2018
Release Version: 02-04
===============================================================================

Introduction:
=============
The folder includes TC9560 drivers and related documents.

Top level directory structure:

  tc9560_drv 
	|-tc9560_usb_class_main.c 
	|-tc9560_usbnet.c
	|-tc9560_gmac.c 
	|-tc9560_ptp.c 
	|-tc9560_reg_rw.c 
	|-tc9560_common.h
	|-tc9560_gmac.h
	|-tc9560_ptp.c
	|-tc9560_reg_rw.h
	|-DWC_ETH_QOS_yapphdr.h
    |-Makefile
    |-readme 
	|-config.ini

Drivers:
========
Note:
1. Root access is required to build & load the driver.

Compilation:
============
1. cd Source_code/driver/hsic/
   # make clean
   # make
 
Insert steps:
=============
1. cd Source_Code/driver/hsic/
2. modprobe usbnet
2. insmod tc9560_usb_class_drv.ko

Remove steps:
==============
1. rmmod tc9560_usb_class_drv.ko
2. rmmod usbnet


Note: tc9560_usbnet.c is based on usbnet.c in the kernel. For this release, the kernel version is 3.19.8. 
If your kernel version is different with 3.19.8, then you need modify tc9560_usbnet.c based on the usbnet.c
in your kernel (driver/net/usb/usbnet.c).