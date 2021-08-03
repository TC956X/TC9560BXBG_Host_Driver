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
 

#ifndef	__TC9560_REG_RW_H
#define	__TC9560_REG_RW_H

//for tc9560_reg_read_i2c and tc9560_reg_write_i2c
//#include "../../test/common/drv_intrf.h"

void tc9560_delay(u32 delay);
int tc9560_read(u32 addr, u32 *val);
int tc9560_write(u32 addr, u32 val);
int tc9560_write_async(u32 addr, u32 val);
int tc9560_reg_read(struct usbnet *dev, u32 addr, u32 *val);
int tc9560_reg_write(struct usbnet *dev, u32 addr, u32 val);
int tc9560_reg_write_async(struct usbnet *dev, u32 addr, u32 val);

#endif //__TC9560_REG_RW_H
