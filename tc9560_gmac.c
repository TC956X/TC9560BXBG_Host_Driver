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
/** \file
 * This file defines the synopsys GMAC device dependent functions.
 * Most of the operations on the GMAC device are available in this file.
 * Functions for initiliasing and accessing MAC/DMA/PHY registers and the DMA descriptors
 * are encapsulated in this file. The functions are platform/host/OS independent.
 * These functions in turn use the low level device dependent (HAL) functions to 
 * access the register space.
 * \internal
 * ------------------------REVISION HISTORY---------------------------------
 * Synopsys                 01/Aug/2007                              Created
 */
 
/*! History:   
 *      18-July-2016 : Initial 
 */
 
#include "tc9560_common.h"
#include "tc9560_gmac.h"

#include <linux/delay.h>

/*Sample Wake-up frame filter configurations*/

u32 synopGMAC_wakeup_filter_config0[] = {
	0x00000000,	// For Filter0 CRC is not computed may be it is 0x0000
	0x00000000,	// For Filter1 CRC is not computed may be it is 0x0000
	0x00000000,	// For Filter2 CRC is not computed may be it is 0x0000
	0x5F5F5F5F, // For Filter3 CRC is based on 0,1,2,3,4,6,8,9,10,11,12,14,16,17,18,19,20,22,24,25,26,27,28,30 bytes from offset
	0x09000000, // Filter 0,1,2 are disabled, Filter3 is enabled and filtering applies to only multicast packets
	0x1C000000, // Filter 0,1,2 (no significance), filter 3 offset is 28 bytes from start of Destination MAC address 
	0x00000000, // No significance of CRC for Filter0 and Filter1
	0xBDCC0000  // No significance of CRC for Filter2, Filter3 CRC is 0xBDCC
};

u32 synopGMAC_wakeup_filter_config1[] = {
	0x00000000,	// For Filter0 CRC is not computed may be it is 0x0000
	0x00000000,	// For Filter1 CRC is not computed may be it is 0x0000
	0x7A7A7A7A,	// For Filter2 CRC is based on 1,3,4,5,6,9,11,12,13,14,17,19,20,21,25,27,28,29,30 bytes from offset
	0x00000000, // For Filter3 CRC is not computed may be it is 0x0000
	0x00010000, // Filter 0,1,3 are disabled, Filter2 is enabled and filtering applies to only unicast packets
	0x00100000, // Filter 0,1,3 (no significance), filter 2 offset is 16 bytes from start of Destination MAC address 
	0x00000000, // No significance of CRC for Filter0 and Filter1
	0x0000A0FE  // No significance of CRC for Filter3, Filter2 CRC is 0xA0FE
};
u32 synopGMAC_wakeup_filter_config2[] = {
	0x00000000,	// For Filter0 CRC is not computed may be it is 0x0000
	0x000000FF,	// For Filter1 CRC is computed on 0,1,2,3,4,5,6,7 bytes from offset
	0x00000000,	// For Filter2 CRC is not computed may be it is 0x0000
	0x00000000, // For Filter3 CRC is not computed may be it is 0x0000
	0x00000100, // Filter 0,2,3 are disabled, Filter 1 is enabled and filtering applies to only unicast packets
	0x0000DF00, // Filter 0,2,3 (no significance), filter 1 offset is 223 bytes from start of Destination MAC address 
	0xDB9E0000, // No significance of CRC for Filter0, Filter1 CRC is 0xDB9E
	0x00000000  // No significance of CRC for Filter2 and Filter3 
};

/*
   The synopGMAC_wakeup_filter_config3[] is a sample configuration for wake up filter. 
   Filter1 is used here
   Filter1 offset is programmed to 50 (0x32)
   Filter1 mask is set to 0x000000FF, indicating First 8 bytes are used by the filter
   Filter1 CRC= 0x7EED this is the CRC computed on data 0x55 0x55 0x55 0x55 0x55 0x55 0x55 0x55

   Refer accompanied software DWC_gmac_crc_example.c for CRC16 generation and how to use the same.
   */

u32 synopGMAC_wakeup_filter_config3[] = {
	0x00000000,	// For Filter0 CRC is not computed may be it is 0x0000
	0x000000FF,	// For Filter1 CRC is computed on 0,1,2,3,4,5,6,7 bytes from offset
	0x00000000,	// For Filter2 CRC is not computed may be it is 0x0000
	0x00000000, // For Filter3 CRC is not computed may be it is 0x0000
	0x00000100, // Filter 0,2,3 are disabled, Filter 1 is enabled and filtering applies to only unicast packets
	0x00003200, // Filter 0,2,3 (no significance), filter 1 offset is 50 bytes from start of Destination MAC address 
	0x7eED0000, // No significance of CRC for Filter0, Filter1 CRC is 0x7EED, 
	0x00000000  // No significance of CRC for Filter2 and Filter3 
};

/**
 * Function to set the MDC clock for mdio transactiona
 *
 * @param[in] pointer to device structure.
 * @param[in] clk divider value.
 * \return Reuturns 0 on success else return the error value.
 */
s32 synopGMAC_set_mdc_clk_div(struct usbnet *dev, u32 clk_div_val)
{
	u32 orig_data;
	u32 ret;
	DBGPR("In synopGMAC_set_mdc_clk_div\n");
	ret = tc9560_reg_read(dev, MAC_GMIIAR_RgOffAddr, &orig_data);
	DBGPR("MAC GMIIAR reg orig value  %x\n",orig_data);

	orig_data &= (~ 0xF00);
	orig_data |= clk_div_val;

	ret = tc9560_reg_write(dev, MAC_GMIIAR_RgOffAddr, orig_data);
//	CHECK(ret, "tc9560_reg_write: GmacGmiiAddr");

	return 0;
}

/**
 * Function to read the Phy register. The access to phy register
 * is a slow process as the data is moved accross MDI/MDO interface
 * @param[in] pointer to Register Base (It is the mac base in our case) .
 * @param[in] PhyBase register is the index of one of supported 32 PHY devices.
 * @param[in] Register offset is the index of one of the 32 phy register.
 * @param[out] u16 data read from the respective phy register (only valid iff return value is 0).
 * \return Returns 0 on success else return the error status.
 */
s32 synopGMAC_read_phy_reg(struct usbnet *dev, u32 PhyBase, u32 RegOffset, u16 *data)
{
	u32 addr;
	u32 loop_variable;
	u32 ret;
	u32 read_val = 0;

	addr = ((PhyBase << GmiiDevShift) & GmiiDevMask) | ((RegOffset << GmiiRegShift) & GmiiRegMask) | (GmiiRead) | (GmiiCsrClk1);
	addr = addr | GmiiBusy ; //Gmii busy bit

	ret = tc9560_reg_write(dev, MAC_GMIIAR_RgOffAddr, addr);
//	CHECK(ret, "tc9560_reg_write: GmacGmiiAddr");

	//Wait till the busy bit gets cleared with in a certain amount of time
	for(loop_variable = 0; loop_variable < 10/* DEFAULT_LOOP_VARIABLE*/; loop_variable++)
	{
		tc9560_reg_read(dev, MAC_GMIIAR_RgOffAddr , &read_val);

		if (!(read_val & GmiiBusy)){
			break;
		}
		tc9560_delay(DEFAULT_DELAY_VARIABLE);
	}
	if(loop_variable < 10/*DEFAULT_LOOP_VARIABLE*/) {
		tc9560_reg_read(dev, MAC_GMIIDR_RgOffAddr , &read_val);
		*data = (u16)(read_val & 0xFFFF);
	}
	else{
		TR("Error::: PHY not responding Busy bit didnot get cleared !!!!!!\n");
		return -ESYNOPGMACPHYERR;
	}


	//    return -ESYNOPGMACNOERR;
	return 0;
}

/**
 * Function to write to the Phy register. The access to phy register
 * is a slow process as the data is moved accross MDI/MDO interface
 * @param[in] pointer to Register Base (It is the mac base in our case) .
 * @param[in] PhyBase register is the index of one of supported 32 PHY devices.
 * @param[in] Register offset is the index of one of the 32 phy register.
 * @param[in] data to be written to the respective phy register.
 * \return Returns 0 on success else return the error status.
 */
s32 synopGMAC_write_phy_reg(struct usbnet *dev, u32 PhyBase, u32 RegOffset, u16 data)
{

#if 1
	u32 addr;
	u32 loop_variable;
	u32 ret, read_val;
	u32 vy_count;

	DBGPR("In synopGMAC_write_phy_reg\n");

	if((RegOffset== 0) && (data & 0x8800))
	{
		printk(KERN_ALERT"PHY reset and power down not supported = %x\n", (u32)data);
		data &= 0x77FF;
	}

  /* wait for any previous MII read/write operation to complete */

  /*Poll Until Poll Condition */
  vy_count = 0;
  while (1) {
    if (vy_count > DEFAULT_LOOP_VARIABLE) {
			return -ESYNOPGMACPHYERR;
    } else {
      vy_count++;
      mdelay(1);
    }
		tc9560_reg_read(dev, MAC_GMIIAR_RgOffAddr, &read_val);
		if (!(read_val & GmiiBusy)){
			break;
		}
		tc9560_delay(DEFAULT_DELAY_VARIABLE);
  }

	ret = tc9560_reg_write(dev, MAC_GMIIDR_RgOffAddr , data);

	tc9560_reg_read(dev, MAC_GMIIAR_RgOffAddr, &addr);
	addr = addr & (u32) (0x12);
 
	//set Gmii clk to 100-150 Mhz and Gmii busy bit
	addr = addr | ((PhyBase << GmiiDevShift) & GmiiDevMask) 
	            | ((RegOffset << GmiiRegShift) & GmiiRegMask) 
							| (GmiiWrite) | (GmiiCsrClk1) | GmiiBusy ; 
	ret = tc9560_reg_write(dev, MAC_GMIIAR_RgOffAddr, addr);
	udelay(10);

	//Wait till the busy bit gets cleared with in a certain amount of time
	for(loop_variable = 0; loop_variable < DEFAULT_LOOP_VARIABLE; loop_variable++)
	{
		tc9560_reg_read(dev, MAC_GMIIAR_RgOffAddr, &read_val);
		if (!(read_val & GmiiBusy)){
			break;
		}
		tc9560_delay(DEFAULT_DELAY_VARIABLE);
	}
	if(loop_variable < DEFAULT_LOOP_VARIABLE)
		return -ESYNOPGMACNOERR;
	else{
		TR("Error::: PHY not responding Busy bit didnot get cleared !!!!!!\n");
		return -ESYNOPGMACPHYERR;
	}
#endif
	return 0;
}

/**
 * Function to read the GMAC IP Version and populates the same in device data structure.
 * @param[in] pointer to synopGMACdevice.
 * \return Always return 0.
 */

s32 synopGMAC_read_version(struct usbnet *dev) 
{	
	u32 data = 0;
	u32 ret = 0;

	DBGPR("In synopGMAC_read_version\n");
	ret = tc9560_reg_read(dev, MAC_VR_RgOffAddr, &data);
//	CHECK(ret, "TC9560_read_cmd: GmacVersion");

	printk("GMAC Version is %08x\n", data);
	return 0;
}

/*Gmac configuration functions*/

/**
 * Sets the GMAC core in Full-Duplex mode. 
 * @param[in] pointer to synopGMACdevice.
 * \return returns void.
 */
void synopGMAC_set_full_duplex(struct usbnet *dev)
{
	DBGPR("In synopGMAC_set_full_duplex\n");
//	tc9560_reg_set_bits(dev, GmacConfig, GmacDuplex);
	MAC_MCR_DM_UdfWr(1);
	return;
}
/**
 * Sets the GMAC core in Half-Duplex mode. 
 * @param[in] pointer to synopGMACdevice.
 * \return returns void.
 */
void synopGMAC_set_half_duplex(struct usbnet *dev)
{
	DBGPR("In synopGMAC_set_half_duplex\n");
	MAC_MCR_DM_UdfWr(0);
	return;
}

/**
 * Disable the reception of frames on GMII/MII.
 * GMAC receive state machine is disabled after completion of reception of current frame.
 * @param[in] pointer to synopGMACdevice.
 * \return returns void.
 */
void synopGMAC_rx_disable(struct usbnet *dev)
{
	DBGPR("func:%s\n",__func__);
	MAC_MCR_RE_UdfWr(0);
	return;
}

/**
 * Disable the transmission of frames on GMII/MII.
 * GMAC transmit state machine is disabled after completion of transmission of current frame.
 * @param[in] pointer to synopGMACdevice.
 * \return returns void.
 */
void synopGMAC_tx_disable(struct usbnet *dev)
{
	DBGPR("func:%s\n",__func__);
	MAC_MCR_TE_UdfWr(0);
	return;
}


/*Receive frame filter configuration functions*/

// TAEC Change Start
/**
 * Sets thespeed of GMAC.
 * This function sets the GMAC config and EVB control reg to set the specific speed defiend. 
 * @param[in] usbnet dev.
 * @param[in] Speed.
 * \return none.
 */
void synopGMAC_set_speed(struct usbnet *dev, u32 speed)
{
	u32 data = 0;
	u32 gmac_config = 0;
	u32 nemacctl_speed_val = 0;

	/* Based on speed auto negotiated by PHY set GMAC registers */
	if (speed == SPEED_1000) {
		gmac_config = 0x0000;
		nemacctl_speed_val = 0;	//TX_CLK = 125MHz
	} else if (speed == SPEED_100) {
		gmac_config = 0xC000;
		nemacctl_speed_val = 2;	//TX_CLK = 25MHz
	} else if (speed == SPEED_10) {
		gmac_config = 0x8000;
		nemacctl_speed_val = 3;	//TX_CLK = 2.5MHz
	} else {
		printk("Auto-negotiation Speed selection is not correct\n"); 
		return;
	}
	/* GMAC Configuration settings */
	tc9560_reg_read(dev, MAC_MCR_RgOffAddr, &data);
	DBGPR("GMAC config reg read = %x\n", data);
	data &= 0xFFFF3FFF;
	data |= gmac_config;
    	tc9560_reg_write(dev, MAC_MCR_RgOffAddr, data);
	data = 0;
	tc9560_reg_read(dev, MAC_MCR_RgOffAddr, &data);
	DBGPR("GMAC config reg read after write = %x\n", data);

	/* NEMACCTL settings*/
	data = 0;
	tc9560_reg_read(dev, NEMACCTL, &data);
	DBGPR("NEMACCTL reg read = %x\n", data);
	data &= ~NEMACCTL_SPEED_MASK;
	data |= nemacctl_speed_val;
	tc9560_reg_write(dev, NEMACCTL, data);
        data = 0;
	tc9560_reg_read(dev, NEMACCTL, &data);
	DBGPR("NEMACCTL reg read after write = %x\n", data);

	return;
}
// TAEC Change End

/**
 * Checks and initialze phy.
 * This function checks whether the phy initialization is complete. 
 * @param[in] pointer to synopGMACdevice.
 * \return 0 if success else returns the error number.
 */
s32 synopGMAC_check_phy_init (struct usbnet *dev, int loopback) 
{	
	u16 data;
	s32 status = -ESYNOPGMACNOERR;		
	s32 loop_count;
	struct tc9560_data *priv = (struct tc9560_data *)dev->data[0];

	DBGPR("func:%s\n",__func__);

//	ret = tc9560_reg_write(dev, MAC_GMIIDR_RgOffAddr, 0x0000B000); /* rgmii: MAC_MDIO_Data */			    if(ret<0) DBGPR("Mac Reg Write Failed");
//	ret = tc9560_reg_write(dev, MAC_GMIIAR_RgOffAddr, 0x00320105); /* AuxCtl: MAC_MDIO_Address */            if(ret<0) DBGPR("Mac Reg Write Failed");
//	ret = tc9560_reg_write(dev, MAC_GMIIDR_RgOffAddr, 0x00001140); /* csr: MAC_MDIO_Data */
//	ret = tc9560_reg_write(dev, MAC_GMIIDR_RgOffAddr, 0x00002100); /* csr: MAC_MDIO_Data */
//	if(ret<0) DBGPR("Mac Reg Write Failed");

//	ret = tc9560_reg_write(dev, MAC_GMIIAR_RgOffAddr, 0x00200105); /* MAC_MDIO_Address */                    if(ret<0) DBGPR("Mac Reg Write Failed");
//	ret = tc9560_reg_write(dev, MAC_GMIIAR_RgOffAddr, 0x0021010D); /* MAC_MDIO_Address */                    if(ret<0) DBGPR("Mac Reg Write Failed");
//	DBGPR("tc9560_dbg: %s priv->phy_id:%d\n", __func__,priv->phy_id);


	loop_count = DEFAULT_LOOP_VARIABLE;
//	loop_count = 1000;
	while(loop_count-- > 0)
	{
		status = synopGMAC_read_phy_reg(dev, priv->phy_id, PHY_STATUS_REG, &data);
		if(status)	
			return status;

		if((data & Mii_AutoNegCmplt) != 0){
			TR("Autonegotiation Complete\n");
			break;
		}
	}

	status = synopGMAC_read_phy_reg(dev, priv->phy_id, PHY_SPECIFIC_STATUS_REG, &data);
	if(status)
		return status;

	DBGPR("PHY_SPECIFIC_STATUS_REG val = 0x%08x\n", data);

	if((data & Mii_phy_status_link_up) == 0){
		printk("TC9560 HSIC Link is Down\n");
		/* Link is down */
		priv->LinkState = LINKDOWN; 
		priv->DuplexMode = -1;
		priv->Speed = 0;
		priv->LoopBackMode = 0; 
		netif_carrier_off(dev->net);
		return -ESYNOPGMACPHYERR;
	}
	else
		priv->LinkState = LINKUP; 

	priv->DuplexMode = (data & Mii_phy_status_full_duplex)  ? FULLDUPLEX: HALFDUPLEX ;

	if(data & Mii_phy_status_speed_1000)
		priv->Speed      =   SPEED_1000;
	else if(data & Mii_phy_status_speed_100)
		priv->Speed      =   SPEED_100;
	else
		priv->Speed      =   SPEED_10;
	

	status = synopGMAC_read_phy_reg(dev, priv->phy_id, PHY_1000BT_CTRL_REG, &data);
	if(status)
		return status;

	if (data & Mii_Master_Mode)
		DBGPR("PHY is currently in MASTER MODE\n");	
	else
		DBGPR("PHY is currently in SLAVE MODE\n");

	printk("TC9560 HSIC Link is Up - %s/%s\n",
		((priv->Speed == SPEED_10) ? "10Mbps": ((priv->Speed == SPEED_100) ? "100Mbps": "1Gbps")),
		(priv->DuplexMode == FULLDUPLEX) ? "Full Duplex": "Half Duplex");

	return -ESYNOPGMACNOERR;
}

/**
 * Sets the Mac address in to GMAC register.
 * This function sets the MAC address to the MAC register in question.
 * @param[in] pointer to synopGMACdevice to populate mac dma and phy addresses.
 * @param[in] Register offset for Mac address high
 * @param[in] Register offset for Mac address low
 * @param[in] buffer containing mac address to be programmed.
 * \return 0 upon success. Error code upon failure.
 */
s32 synopGMAC_set_mac_addr(struct usbnet *dev, u32 MacHigh, u32 MacLow, u8 *MacAddr)
{
	u32 data;

	DBGPR("func:%s\n",__func__);
	data = (MacAddr[5] << 8) | MacAddr[4];
	tc9560_reg_write(dev,MacHigh,data);
	tc9560_reg_write(dev,MacHigh,((1<<31)|data));
	data = (MacAddr[3] << 24) | (MacAddr[2] << 16) | (MacAddr[1] << 8) | MacAddr[0] ;
	tc9560_reg_write(dev,MacLow,data);
	return 0;
}


/**
 * Get the Mac address in to the address specified.
 * The mac register contents are read and written to buffer passed.
 * @param[in] pointer to synopGMACdevice to populate mac dma and phy addresses.
 * @param[in] Register offset for Mac address high
 * @param[in] Register offset for Mac address low
 * @param[out] buffer containing the device mac address.
 * \return 0 upon success. Error code upon failure.
 */
s32 synopGMAC_get_mac_addr(struct usbnet *dev, u32 MacHigh, u32 MacLow, u8 *MacAddr)
{
	u32 data;
	u32 ret;

	ret = tc9560_reg_read(dev, MacHigh, &data);
//	CHECK(ret, "TC9560_read_cmd: MacHigh");

	MacAddr[5] = (data >> 8) & 0xff;
	MacAddr[4] = (data)        & 0xff;

	ret = tc9560_reg_read(dev, MacLow, &data);
//	CHECK(ret, "TC9560_read_cmd: MacLow");
//	DBGPR("Mac low address %x\n",data);

	MacAddr[3] = (data >> 24) & 0xff;
	MacAddr[2] = (data >> 16) & 0xff;
	MacAddr[1] = (data >> 8 ) & 0xff;
	MacAddr[0] = (data )      & 0xff;

	return 0;
}

/*******************PMT APIs***************************************/

/**
 * Populates the remote wakeup frame registers.
 * Consecutive 8 writes to GmacWakeupAddr writes the wakeup frame filter registers.
 * Before commensing a new write, frame filter pointer is reset to 0x0000.
 * A small delay is introduced to allow frame filter pointer reset operation.
 * @param[in] pointer to synopGMACdevice.
 * @param[in] pointer to frame filter contents array.
 * \return returns void.
 */
void synopGMAC_write_wakeup_frame_register(struct usbnet *dev, u32 * filter_contents)
{
	s32 i;
	MAC_PMTCSR_RWKFILTRST_UdfWr(1);
//	tc9560_reg_set_bits(dev,GmacPmtCtrlStatus,GmacPmtFrmFilterPtrReset);
	tc9560_delay(10);	
	for(i =0; i<WAKEUP_REG_LENGTH; i++)
		tc9560_reg_write(dev, MAC_RWPFFR_RgOffAddr ,  *(filter_contents + i));
	return;

}
/*******************PMT APIs***************************************/

/*******************MMC APIs***************************************/
/*******************Ip checksum offloading APIs***************************************/

/**
 * Enables the ip checksum offloading in receive path.
 * When set GMAC calculates 16 bit 1's complement of all received ethernet frame payload.
 * It also checks IPv4 Header checksum is correct. GMAC core appends the 16 bit checksum calculated
 * for payload of IP datagram and appends it to Ethernet frame transferred to the application.
 * @param[in] pointer to synopGMACdevice.
 * \return returns void.
 */
void synopGMAC_enable_rx_chksum_offload(struct usbnet *dev)
{
	DBGPR("func:%s\n",__func__);
//	tc9560_reg_set_bits(dev,GmacConfig,GmacRxIpcOffload);
	MAC_MCR_IPC_UdfWr(1);
	return;
}

