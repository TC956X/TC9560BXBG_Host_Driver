/* ============================================================================
 * PROJECT: TC9560
 * Copyright (C) 2018  Toshiba Electronic Devices & Storage Corporation
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 * ========================================================================= */
 
/*! History:   
 *      18-July-2016 : Initial 
 */
 

#include "tc9560_common.h"
#include "tc9560_ptp.h"
//#include "tc9560_reg_rw.h"

//#define TC9560_ACCESS_MAC 1
#define DEBUG_REG_RDWR

extern struct tc9560_ptp_data tc9560_pdata;
extern struct tc9560_data *pdata_phc;

/**
  * This is a wrapper function for platform dependent delay 
  * Take care while passing the argument to this function 
  * @param[in] buffer pointer to be freed
  */
void tc9560_delay(u32 delay)
{
	while (delay--);
	return;
}

static int tc9560_reg_read_usb(struct usbnet *dev, u8 cmd, u16 value, u16 index, u16 size, void *data)
{
	int ret = 0;
//	BUG_ON(!dev);
	
	ret = usbnet_read_cmd_nopm(dev, cmd, USB_DIR_IN | USB_TYPE_VENDOR |
			 USB_RECIP_DEVICE, value, index, data, size);

	if (unlikely(ret <= 0))
		netdev_warn(dev->net, "Failed to read reg index 0x%04x: %d\n",
			    index, ret);
	return ret;
}

static int tc9560_reg_write_usb(struct usbnet *dev, u8 cmd, u16 value, u16 index, u16 size, void *data)
{
	int ret = 0;
//	BUG_ON(!dev);
	
	ret = usbnet_write_cmd_nopm(dev, cmd, USB_DIR_OUT | USB_TYPE_VENDOR |
			 USB_RECIP_DEVICE, value, index, data, size);

	if (unlikely(ret <= 0))
		netdev_warn(dev->net, "Failed to write reg index 0x%04x: %d\n",
			    index, ret);
	return ret;
}

static int tc9560_reg_write_usb_async(struct usbnet *dev, u8 cmd, u16 value, u16 index, u16 size, void *data)
{
	int ret;
//	BUG_ON(!dev);

	ret = usbnet_write_cmd_async(dev, cmd, USB_DIR_OUT | USB_TYPE_VENDOR |
		 USB_RECIP_DEVICE, value, index, data, size);

	if (unlikely(ret < 0))
		netdev_warn(dev->net, "Failed to write reg index 0x%04x: %d\n",
				index, ret);
	return ret;
}

int tc9560_reg_read(struct usbnet *dev, u32 addr, u32 *val)
{
	int ret = 0;
    	ret = tc9560_reg_read_usb(dev, TC9560_ACCESS_MAC, addr, 4, 4, val);
	if(ret < 0)
		printk("ERROR: tc9560_reg_read: failed. Addr: 0x%08x, Val: 0x%08x.\n", addr, *val);
	return ret;
}

int tc9560_reg_write(struct usbnet *dev, u32 addr, u32 val)
{
	int ret;
    ret = tc9560_reg_write_usb(dev, TC9560_ACCESS_MAC, addr, 4, 4, &val);
//    printk("Reg access interface is not defined!!!\n");
//	if(ret < 0)
//		printk("ERROR: tc9560_reg_write: failed. Addr: 0x%08x, Val: 0x%08x.\n", addr, val);
    return ret;
}

int tc9560_reg_write_async(struct usbnet *dev, u32 addr, u32 val)
{
	int ret;
    ret = tc9560_reg_write_usb_async(dev, TC9560_ACCESS_MAC, addr, 4, 4, &val);
//    printk("Reg access interface is not defined!!!\n");
//	if(ret < 0)
//		printk("ERROR: tc9560_reg_write: failed. Addr: 0x%08x, Val: 0x%08x.\n", addr, val);
    return ret;
}

int tc9560_read(u32 addr, u32 *val)
{
//	struct usbnet *dev;
	int ret;
	ret = tc9560_reg_read(tc9560_pdata.dev, addr, val);
	if(ret < 0)
		printk("ERROR: tc9560_read: failed. Addr: 0x%08x, Val: 0x%08x.\n", addr, *val);
	return ret;
}


int tc9560_write(u32 addr, u32 val)
{
//	struct usbnet *dev;
	int ret;
	ret = tc9560_reg_write(tc9560_pdata.dev, addr, val);
	if(ret < 0)
		printk("ERROR: tc9560_reg_write: failed. Addr: 0x%08x, Val: 0x%08x.\n", addr, val);
	return ret;
}

int tc9560_write_async(u32 addr, u32 val)
{
//	struct usbnet *dev;
	int ret;
	ret = tc9560_reg_write_async(tc9560_pdata.dev, addr, val);
	if(ret < 0)
		printk("ERROR: tc9560_reg_write: failed. Addr: 0x%08x, Val: 0x%08x.\n", addr, val);
	return ret;
}



