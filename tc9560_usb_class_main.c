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
#define IOCTL_DEFINE 0

#if IOCTL_DEFINE
#define ALLOC_SIZE      1512
#endif 
 
#include "tc9560_common.h"
#include "tc9560_ptp.h"
#include <linux/workqueue.h>
#include <linux/time.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/rwlock.h>
#include <linux/usb/hcd.h>
#include <linux/ctype.h>

#include "tc9560_usbnet.c"

#define DRV_VERSION	"V_02-04"
#define RX_URB_SIZE (1024 * 4) 

#define DEV_NAME	"TC9560_USB_CLASS_DRIVER"
#define FW_VERSION	"v1.2"


int32_t nic_speed;
struct  tc9560_ptp_data tc9560_pdata;




static int msg_enable;
module_param(msg_enable, int, 0);
MODULE_PARM_DESC(msg_enable, "usbnet msg_enable");

static int cfg;
module_param(cfg, int, 0);
MODULE_PARM_DESC(cfg, "Configuration selection");


struct api_context{
	struct completion done;
	int	status;
};

#ifndef TC9560_DRV_TEST_LOOPBACK
static struct timer_list synopGMAC_cable_unplug_timer;
#endif
static struct delayed_work task;

struct usb_device *device;
static struct usb_class_driver class;
struct tc9560_usb_device tc9560_dev;
unsigned int phy_loopback_mode;

struct tc9560_data *pdata_phc;

#if (IOCTL_DEFINE == 1)
static struct urb *tc9560_urb_alloc[20];
static uint8_t *dma_buf[20];
static struct tc9560_urb{
         uint32_t tc9560_i;
         struct semaphore tc9560_sem;
}tc9560_urb_context;
#endif

typedef struct
{
	char mdio_key[32];
	unsigned short mdio_key_len;
	char mac_key[32];
	unsigned short mac_key_len;
	unsigned short mac_str_len;
	char mac_str_def[20];
} config_param_list_t;

static const config_param_list_t config_param_list[] = {
{"MDIOBUSID",9,"MAC_ID", 6, 18, "00:00:00:00:00:00"},
};

#define CONFIG_PARAM_NUM (sizeof(config_param_list)/sizeof(config_param_list[0]))

unsigned short mdio_bus_id = 0;
static unsigned char dev_addr[6] = { 0xE8, 0xE0, 0xB7, 0xB5, 0x7D, 0xF8};   

/*!
 * \brief API to kernel read from file
 *
 * \param[in] file   - pointer to file descriptor
 * \param[in] offset - Offset of file to start read
 * \param[in] size   - Size of the buffer to be read
 *
 * \param[out] data   - File data buffer
 *
 *
 * \return integer
 *
 * \retval 0 on success & -ve number on failure.
 */
static int file_read(struct file* file, unsigned long long offset, unsigned char* data, unsigned int size) {
    mm_segment_t oldfs;
    int ret;

    oldfs = get_fs();
    set_fs(get_ds());

    ret = vfs_read(file, data, size, &offset);

    set_fs(oldfs);
    return ret;
}

/*!
 * \brief API to validate MAC ID
 *
 * \param[in] char *s - pointer to MAC ID string
 * 
 * \return boolean
 *
 * \retval true on success and false on failure.
 */
bool isMAC(char *s) {
    int i=0;
    if (s == NULL)
	return false;

    for(i = 0; i < 17; i++) {
        if(i % 3 != 2 && !isxdigit(s[i]))
            return false;
        if(i % 3 == 2 && s[i] != ':')
            return false;
    }
    return true;
}

/*!
 * \brief API to extract MAC ID from given string
 *
 * \param[in] char *string - pointer to MAC ID string
 * 
 * \return None
 */
void extract_macid(char *string)
{
	char *token_m = NULL;
	int j = 0;
        int mac_id = 0;

	/* Extract MAC ID byte by byte */
	token_m = strsep(&string, ":");
	while(token_m != NULL) {
		sscanf(token_m, "%x", &mac_id);
		dev_addr[j++] = mac_id;
		token_m = strsep(&string, ":");
	}
}

/*!
 * \brief API to parse and extract the user configured MAC ID
 *
 * \param[in] file_buf - Pointer to file data buffer
 *
 * \return boolean
 *
 * \return - True on Success and False in failure 
 */
static bool lookfor_macid(char *file_buf)
{
	char *string = NULL, *token_n = NULL, *token_s = NULL, *token_m = NULL;
	bool status = false;
	int tc9560_device_no= 0;

	string = file_buf;
	/* Parse Line-0 */	
	token_n = strsep(&string, "\n");
	while (token_n != NULL) {

		/* Check if line is enabled */
		if (token_n[0] != '#') {
			/* Extract the token based space character */
			token_s = strsep(&token_n, " ");
			if (token_s != NULL) {
			if (strncmp (token_s, config_param_list[0].mdio_key, 9) == 0 ) {
					token_s = strsep(&token_n, " ");
					token_m = strsep(&token_s, ":");
					sscanf(token_m, "%d", &tc9560_device_no);
					if (tc9560_device_no != mdio_bus_id){
						if ((token_n = strsep(&string, "\n")) == NULL)
							break;
						continue;
					}
				}
			}
			
			/* Extract the token based space character */
			token_s = strsep(&token_n, " ");
			if (token_s != NULL) {
				/* Compare if parsed string matches with key listed in configuration table */
				if (strncmp (token_s, config_param_list[0].mac_key, 6) == 0 ) {

					DBGPR("MAC_ID Key is found\n");
					/* Read next word */
					token_s = strsep(&token_n, " \n");
					if (token_s != NULL) {

						/* Check if MAC ID length  and MAC ID is valid */
						if ((isMAC(token_s) == true) && (strlen(token_s) ==  config_param_list[0].mac_str_len)) {
							/* If user configured MAC ID is valid,  assign default MAC ID */
							extract_macid(token_s);
							status = true;
						} else {
							DBGPR( "Valid Mac ID not found\n");
						}
					}
				}
			}
		}
		/* Read next lile */
                if ((token_n = strsep(&string, "\n")) == NULL)
			break;
		
	}
	return status;
}

/*!
 * \brief Parse the user configuration file for various config
 *
 * \param[in] None
 *
 * \return None
 *
 */
static void parse_config_file(void)
{
	struct file *filep = NULL;
	char data[1000]={0};
	mm_segment_t oldfs;
	int ret, flags = O_RDONLY, i = 0;

	oldfs = get_fs();
	set_fs(get_ds());
	filep = filp_open("config.ini", flags, 0600);
	set_fs(oldfs);
	if(IS_ERR(filep)) {
		DBGPR( "Mac configuration file not found\n");
		DBGPR( "Using Default MAC Address\n");
		return;
	}
	else  {
		/* Parse the file */
		ret = file_read(filep, 0, data, 1000);
		for (i = 0; i < CONFIG_PARAM_NUM; i++) {
			if (strstr ((const char *)data, config_param_list[i].mdio_key)) {
				DBGPR("Pattern Match\n");
				if (strncmp(config_param_list[i].mdio_key, "MDIOBUSID", 9) == 0) {
					/* MAC ID Configuration */
					DBGPR("MAC_ID Configuration\n");
					if (lookfor_macid(data) == false) {
						//extract_macid ((char *)config_param_list[i].str_def);
					}
		}
			}
		}
	}

	filp_close(filep, NULL);

	return;
}

/*!
 * \brief This sequence is used to select Tx Scheduling Algorithm for AVB feature for Queue[1 - 7]
 * \param[in] avb_algo
 * \return Success or Failure
 * \retval  0 Success
 * \retval -1 Failure
 */
static int set_avb_algorithm(u32 chInx, u8 avb_algo)
{

  MTL_QECR_AVALG_UdfWr(chInx, avb_algo);

  return Y_SUCCESS;
}


/*!
 * \brief This sequence is used to configure credit-control for Queue[1 - 7]
 * \param[in] chInx
 * \return Success or Failure
 * \retval  0 Success
 * \retval -1 Failure
 */
static int config_credit_control(u32 chInx, u32 cc)
{

  MTL_QECR_CC_UdfWr(chInx, cc);

  return Y_SUCCESS;
}


/*!
 * \brief This sequence is used to set tx queue operating mode for Queue[0 - 7]
 * \param[in] chInx
 * \param[in] q_mode
 * \return Success or Failure
 * \retval  0 Success
 * \retval -1 Failure
 */
static int set_tx_queue_operating_mode(u32 chInx,
                                       u32 q_mode)
{

  MTL_QTOMR_TXQEN_UdfWr(chInx, q_mode);

  return Y_SUCCESS;
}


/*!
 * \brief This sequence is used to configure send slope credit value
 * required for the credit-based shaper alogorithm for Queue[1 - 7]
 * \param[in] chInx
 * \param[in] sendSlope
 * \return Success or Failure
 * \retval  0 Success
 * \retval -1 Failure
 */
static int config_send_slope(u32 chInx,
                          u32 sendSlope)
{
  DBGPR("send slop  %08x\n",sendSlope);
  MTL_QSSCR_SSC_UdfWr(chInx, sendSlope);

  return Y_SUCCESS;
}

/*!
 * \brief This sequence is used to configure idle slope credit value
 * required for the credit-based shaper alogorithm for Queue[1 - 7]
 * \param[in] chInx
 * \param[in] idleSlope
 * \return Success or Failure
 * \retval  0 Success
 * \retval -1 Failure
 */
static int config_idle_slope(u32 chInx,
                          u32 idleSlope)
{
  DBGPR("Idle slop  %08x\n",idleSlope);
  MTL_QW_ISCQW_UdfWr(chInx, idleSlope);

  return Y_SUCCESS;
}



/*!
 * \brief This sequence is used to configure low credit value
 * required for the credit-based shaper alogorithm for Queue[1 - 7]
 * \param[in] chInx
 * \param[in] lowCredit
 * \return Success or Failure
 * \retval  0 Success
 * \retval -1 Failure
 */
static int config_low_credit(u32 chInx,
                        u32 lowCredit)
{
        int lowCredit_neg = lowCredit;
        DBGPR(KERN_CRIT "lowCreidt = %08x lowCredit_neg:%08x\n",
                        lowCredit, lowCredit_neg);
        MTL_QLCR_LC_UdfWr(chInx, lowCredit_neg);

  MTL_QLCR_LC_UdfWr(chInx, lowCredit);

  return Y_SUCCESS;
}




/*!
 * \brief This sequence is used to configure high credit value required
 * for the credit-based shaper alogorithm for Queue[1 - 7]
 * \param[in] chInx
 * \param[in] hiCredit
 * \return Success or Failure
 * \retval  0 Success
 * \retval -1 Failure
 */
static int config_high_credit(u32 chInx,
                           u32 hiCredit)
{
   DBGPR(KERN_CRIT "hiCreidt = %08x \n",hiCredit);

  MTL_QHCR_HC_UdfWr(chInx, hiCredit);

  return Y_SUCCESS;
}

/*!
* \brief This sequence is used to select perfect/inverse matching for L2 DA
* \param[in] perfect_inverse_match
* \return Success or Failure
* \retval  0 Success
* \retval -1 Failure
*/

static int config_l2_da_perfect_inverse_match(struct tc9560_data *priv, int perfect_inverse_match)
{
	priv->MAC_Packet_Filter = (priv->MAC_Packet_Filter & (MAC_MPFR_RES_Wr_Mask_22))|((( 0) & (MAC_MPFR_Mask_22))<<22);
	priv->MAC_Packet_Filter = (priv->MAC_Packet_Filter & (MAC_MPFR_RES_Wr_Mask_17))|((( 0) & (MAC_MPFR_Mask_17))<<17);
	priv->MAC_Packet_Filter = (priv->MAC_Packet_Filter & (MAC_MPFR_RES_Wr_Mask_11))|((( 0) & (MAC_MPFR_Mask_11))<<11);
	priv->MAC_Packet_Filter = ((priv->MAC_Packet_Filter & MAC_MPFR_DAIF_Wr_Mask) | ((perfect_inverse_match & MAC_MPFR_DAIF_Mask)<<3));
	MAC_MPFR_RgWr_async(priv->MAC_Packet_Filter);

  return Y_SUCCESS;
}

/*!
* \brief This sequence is used to configure hash table register for
* hash address filtering
* \param[in] idx
* \param[in] data
* \return Success or Failure
* \retval  0 Success
* \retval -1 Failure
*/

static int update_hash_table_reg(int idx, unsigned int data)
{

  MAC_HTR_RgWr_async(idx, data);

  return Y_SUCCESS;
}

/*!
* \brief This sequence is used to configure MAC in differnet pkt processing
* modes like promiscuous, multicast, unicast, hash unicast/multicast.
* \param[in] pr_mode
* \param[in] huc_mode
* \param[in] hmc_mode
* \param[in] pm_mode
* \param[in] hpf_mode
* \return Success or Failure
* \retval  0 Success
* \retval -1 Failure
*/

static int config_mac_pkt_filter_reg(struct tc9560_data *priv, unsigned char pr_mode, unsigned char huc_mode, unsigned char hmc_mode, unsigned char pm_mode, unsigned char hpf_mode)
{
  /* configure device in differnet modes */
  /* promiscuous, hash unicast, hash multicast, */
  /* all multicast and perfect/hash filtering mode. */

  priv->MAC_Packet_Filter &= (unsigned int)(0x003003e8);
  priv->MAC_Packet_Filter |= ((pr_mode) << 31) | ((huc_mode) << 1) | ((hmc_mode) << 2) | ((pm_mode) << 4) | ((hpf_mode) << 10);

  MAC_MPFR_RgWr_async(priv->MAC_Packet_Filter);
  
  DBGPR("PKT Filter Value : %x\n", priv->MAC_Packet_Filter);
  return Y_SUCCESS;
}

/*!
* \brief This sequence is used to update the MAC address in 3 to 31 MAC
* address Low and High register(3-31) for L2 layer filtering.
*
* MAC Address Registers [0] [1] [2] should not be used for Perfect filtering.
* OS may override valid MAC Addresses (when multiple MACs are enabled). 
* 
* \param[in] idx
* \param[in] addr
* \return Success or Failure
* \retval  0 Success
* \retval -1 Failure
*/

static int update_mac_addr3_31_low_high_reg(int idx, unsigned char addr[])
{
	unsigned int HR = 0x0000FFFF & ((0x1<<17)|(addr[4] | (addr[5] << 8)));

	/* we have to configure each MAC add given from App but 0,1,2 are reserved so we have to update from 3 onwards here idx always start from 1*/
	MAC_MA1_31LR_RgWr_async(idx+2, (addr[0] | (addr[1] << 8) | (addr[2] << 16) | (addr[3] << 24)));
	MAC_MA1_31HR_RgWr_async(idx+2, HR);
	MAC_MA1_31HR_RgWr_async(idx+2, HR | (0x1<<31));

	return Y_SUCCESS;
}

/*!
 *  * \details This function is invoked by ioctl function when the user issues an
 *  ioctl command to select the AVB algorithm. This function also configures other
 *  parameters like send and idle slope, high and low credit.
 *
 *  \param[in] pdata – pointer to private data structure.
 *  \param[in] req – pointer to ioctl data structure.
 *
 *  \return void
 *
 *  \retval none
 */
static int DWC_ETH_QOS_program_avb_algorithm(struct ifr_data_struct *req)
{
        struct DWC_ETH_QOS_avb_algorithm l_avb_struct, *u_avb_struct =
                (struct DWC_ETH_QOS_avb_algorithm *)req->ptr;

//        DBGPR("-->DWC_ETH_QOS_program_avb_algorithm\n");

        if(copy_from_user(&l_avb_struct, u_avb_struct,
                                sizeof(struct DWC_ETH_QOS_avb_algorithm)))
                printk(KERN_ALERT "Failed to fetch AVB Struct info from user\n");

        set_tx_queue_operating_mode(l_avb_struct.chInx,
                (u32)l_avb_struct.op_mode);
        set_avb_algorithm(l_avb_struct.chInx, l_avb_struct.algorithm);
        config_credit_control(l_avb_struct.chInx, l_avb_struct.cc);
        config_send_slope(l_avb_struct.chInx, l_avb_struct.send_slope);
        config_idle_slope(l_avb_struct.chInx, l_avb_struct.idle_slope);
        config_high_credit(l_avb_struct.chInx, l_avb_struct.hi_credit);
        config_low_credit(l_avb_struct.chInx, l_avb_struct.low_credit);

        return Y_SUCCESS;
}

/*!
 * \brief This sequence is used to write into phy registers
 * \param[in] phy_id
 * \param[in] phy_reg
 * \param[in] phy_reg_data
 * \return Success or Failure
 * \retval  0 Success
 * \retval -1 Failure
 */
static int write_phy_regs(int phy_id, int phy_reg, int phy_reg_data)
{

        unsigned long retryCount = 1000;
        unsigned long vy_count;
        volatile unsigned long varMAC_GMIIAR;


        /* wait for any previous MII read/write operation to complete */

        /*Poll Until Poll Condition */
        vy_count = 0;
        while (1) {
                if (vy_count > retryCount) {
                        return -Y_FAILURE;
                } else {
                        vy_count++;
                        mdelay(1);
                }
                MAC_GMIIAR_RgRd(varMAC_GMIIAR);
                if (GET_VALUE(varMAC_GMIIAR, MAC_GMIIAR_GB_LPOS, MAC_GMIIAR_GB_HPOS) == 0) {
                        break;
                }
        }
        /* write the data */
        MAC_GMIIDR_GD_UdfWr(phy_reg_data);
        /* initiate the MII write operation by updating desired */
        /* phy address/id (0 - 31) */
        /* phy register offset */
        /* CSR Clock Range (20 - 35MHz) */
        /* Select write operation */
        /* set busy bit */
        MAC_GMIIAR_RgRd(varMAC_GMIIAR);
        varMAC_GMIIAR = varMAC_GMIIAR & (unsigned long) (0x12);
        varMAC_GMIIAR =
            varMAC_GMIIAR | ((phy_id) << 21) | ((phy_reg) << 16) | ((0x2) << 8)
            | ((0x1) << 2) | ((0x1) << 0);
        MAC_GMIIAR_RgWr(varMAC_GMIIAR);

        /*DELAY IMPLEMENTATION USING udelay() */
        udelay(10);
        /* wait for MII write operation to complete */

        /*Poll Until Poll Condition */
        vy_count = 0;
        while (1) {
                if (vy_count > retryCount) {
                        return -Y_FAILURE;
                } else {
                        vy_count++;
                        mdelay(1);
                }
                MAC_GMIIAR_RgRd(varMAC_GMIIAR);
                if (GET_VALUE(varMAC_GMIIAR, MAC_GMIIAR_GB_LPOS, MAC_GMIIAR_GB_HPOS) == 0) {
                        break;
                }
        }

        return Y_SUCCESS;
}



/*!
 * \brief This sequence is used to read the phy registers
 * \param[in] phy_id
 * \param[in] phy_reg
 * \param[out] phy_reg_data
 * \return Success or Failure
 * \retval  0 Success
 * \retval -1 Failure
 */
static int read_phy_regs(int phy_id, int phy_reg, int *phy_reg_data)
{
        unsigned long retryCount = 1000;
        unsigned long vy_count;
        volatile unsigned long varMAC_GMIIAR;
        unsigned long varMAC_GMIIDR;

        /* wait for any previous MII read/write operation to complete */

        /*Poll Until Poll Condition */
        vy_count = 0;
        while (1) {
                if (vy_count > retryCount) {
                        return -Y_FAILURE;
                } else {
                        vy_count++;
                        mdelay(1);
                }
                MAC_GMIIAR_RgRd(varMAC_GMIIAR);
                if (GET_VALUE(varMAC_GMIIAR, MAC_GMIIAR_GB_LPOS, MAC_GMIIAR_GB_HPOS) == 0) {
                        break;
                }
        }
        /* initiate the MII read operation by updating desired */
        /* phy address/id (0 - 31) */
        /* phy register offset */
        /* CSR Clock Range (20 - 35MHz) */
        /* Select read operation */
        /* set busy bit */
        MAC_GMIIAR_RgRd(varMAC_GMIIAR);
        varMAC_GMIIAR = varMAC_GMIIAR & (unsigned long) (0x12);
        varMAC_GMIIAR =
            varMAC_GMIIAR | ((phy_id) << 21) | ((phy_reg) << 16) | ((0x2) << 8)
            | ((0x3) << 2) | ((0x1) << 0);
        MAC_GMIIAR_RgWr(varMAC_GMIIAR);

        /*DELAY IMPLEMENTATION USING udelay() */
        udelay(10);
        /* wait for MII write operation to complete */

        /*Poll Until Poll Condition */
        vy_count = 0;
        while (1) {
                if (vy_count > retryCount) {
                        return -Y_FAILURE;
                } else {
                        vy_count++;
                        mdelay(1);
                }
                MAC_GMIIAR_RgRd(varMAC_GMIIAR);
                if (GET_VALUE(varMAC_GMIIAR, MAC_GMIIAR_GB_LPOS, MAC_GMIIAR_GB_HPOS) == 0) {
                        break;
                }
        }
        /* read the data */
        MAC_GMIIDR_RgRd(varMAC_GMIIDR);
        *phy_reg_data =
            GET_VALUE(varMAC_GMIIDR, MAC_GMIIDR_GD_LPOS, MAC_GMIIDR_GD_HPOS);

        return Y_SUCCESS;
}


/*!
 * \brief write MII PHY register, function called by the driver alone
 * 
 * \details Writes MII registers through the API write_phy_reg where the
 * related MAC registers can be configured.
 * 
 * \param[in] pdata - pointer to driver private data structure.
 * \param[in] phyaddr - the phy address to write
 * \param[in] phyreg - the phy regiester id
 * 
 * to write
 * \param[out] phydata - actual data to be written into the phy registers
 * 
 * \return void
 * 
 * \retval  0 - successfully read data from register
 * \retval -1 - error occurred
 * \retval  1 - if the feature is not defined.
 */
int DWC_ETH_QOS_mdio_write_direct(int phyaddr, int phyreg, int phydata)
{
        int phy_reg_write_status;

        phy_reg_write_status = write_phy_regs(phyaddr, phyreg, phydata);
        phy_reg_write_status = 1;
        
        return phy_reg_write_status;
}


/*!
 * \brief read MII PHY register, function called by the driver alone
 * 
 * \details Read MII registers through the API read_phy_reg where the
 * related MAC registers can be configured.
 * 
 * \param[in] phyaddr - the phy address to read
 * \param[in] phyreg - the phy regiester id to read
 * \param[out] phydata - pointer to the value that is read from the phy registers
 * 
 * \return int
 * 
 * \retval  0 - successfully read data from register
 * \retval -1 - error occurred
 * \retval  1 - if the feature is not defined.
 */
int DWC_ETH_QOS_mdio_read_direct(int phyaddr, int phyreg, int *phydata)
{
        int phy_reg_read_status;

        TR("--> DWC_ETH_QOS_mdio_read_direct\n");

	phy_reg_read_status = read_phy_regs(phyaddr, phyreg, phydata);
	phy_reg_read_status = 1;

	TR("<-- DWC_ETH_QOS_mdio_read_direct\n");

        return phy_reg_read_status;
}



/*!
 *  \details This function is invoked by ioctl function when user issues
 *  an ioctl command to enable/disable phy loopback mode.
 *    
 *  \param[in] dev pointer to net device structure.
 *  \param[in] flags flag to indicate whether mac loopback mode to be
 *  enabled/disabled.
 *  
 *  \return integer
 *  
 *  \retval zero on success and -ve number on failure.
 */
static int DWC_ETH_QOS_config_phy_loopback_mode(unsigned int flags)
{
    int ret = 0, regval;
	int phyaddr = 0x01;

    TR("-->DWC_ETH_QOS_config_phy_loopback_mode\n");
	TR("value of flag %d\n",flags);

    if (flags && phy_loopback_mode) {
            TR(KERN_ALERT
                    "PHY loopback mode is already enabled\n");
            return -EINVAL;
    }
    if (!flags && !phy_loopback_mode) {
            TR(KERN_ALERT
                    "PHY loopback mode is already disabled\n");
            return -EINVAL;
    }
    phy_loopback_mode = !!flags;

    DWC_ETH_QOS_mdio_read_direct(phyaddr, MII_BMCR, &regval);
    regval = (regval & (~(1<<14))) | (flags<<14);
    DWC_ETH_QOS_mdio_write_direct(phyaddr, MII_BMCR, regval);

    TR(KERN_ALERT "Succesfully %s PHY loopback mode\n",
            (flags ? "enabled" : "disabled"));

    TR("<--DWC_ETH_QOS_config_phy_loopback_mode\n");

    return ret;
}

/*!
 * \details This function is invoked by ioctl function when user issues an
 * ioctl command to configure L2 destination addressing filtering mode. This
 * function dose following,
 * - selects perfect/hash filtering.
 * - selects perfect/inverse matching.
 *
 * \param[in] dev – pointer to net device structure.
 * \param[in] req – pointer to IOCTL specific structure.
 *
 * \return integer
 *
 * \retval zero on success and -ve number on failure.
 */
static int DWC_ETH_QOS_confing_l2_da_filter(struct net_device *net,	struct ifr_data_struct *req)
{
	struct usbnet *dev = netdev_priv(net);
	struct tc9560_data *priv = (struct tc9560_data *)dev->data[0]; 
	
	struct DWC_ETH_QOS_l2_da_filter *u_l2_da_filter =
	  (struct DWC_ETH_QOS_l2_da_filter *)req->ptr;
	struct DWC_ETH_QOS_l2_da_filter l_l2_da_filter;
	int ret = 0;

	DBGPR("-->DWC_ETH_QOS_confing_l2_da_filter\n");

	if (copy_from_user(&l_l2_da_filter, u_l2_da_filter,
	      sizeof(struct DWC_ETH_QOS_l2_da_filter)))
		return - EFAULT;

	if (l_l2_da_filter.perfect_hash) {
			priv->l2_filtering_mode = 1;
	} else {
			priv->l2_filtering_mode = 0;
	}

	/* configure L2 DA perfect/inverse_matching */
	config_l2_da_perfect_inverse_match(priv,l_l2_da_filter.perfect_inverse_match);

	DBGPR("Successfully selected L2 %s filtering and %s DA matching\n",
		(l_l2_da_filter.perfect_hash ? "HASH" : "PERFECT"),
		(l_l2_da_filter.perfect_inverse_match ? "INVERSE" : "PERFECT"));

	DBGPR("<--DWC_ETH_QOS_confing_l2_da_filter\n");

	return ret;
}

/*!
 * \brief This function is used to enable or disable the TC9560 Wrapper Timestamp Packet Blocking feature
 * \param[in] ena_dis, 1=enable, 0=disable
 * \return Success or Failure
 * \retval  0 Success
 * \retval -1 Failure
 */
static int tc9560_wrap_ts_ignore_config(int ena_dis)
{
    int rd_val;

    ETH_AVB_WRAPPER_TS_CTRL_UdfWr(ena_dis);
    ETH_AVB_WRAPPER_TS_CTRL_UdfRd(rd_val);

    if(rd_val != ena_dis)
    {
        printk(KERN_ALERT "ERROR: TC9560 Wrapper Timestamp Enable Feature wr_val:0x%x, rd_val:0x%x \n", ena_dis, rd_val);
        return -Y_SUCCESS;
    }else{
        DBGPR("TC9560 Wrapper Timestamp Enable Feature : wr_val:0x%x, rd_val:0x%x \n", ena_dis, rd_val);
        return Y_SUCCESS;
    }
}


/*!
 *  \brief Register read/write function called by driver IOCTL routine
 *   	 This function doesn't do address validation, it is application's resposibility 
 *       to make sure a valid address is passed in.
 *    
 *  \param[in] pdata : pointer to private data structure.
 *  \param[in] req : pointer to ioctl structure.
 *       req->flags = 0: TC9560 Register Read
 *  	 req->flags = 1: TC9560 Register Write
 *       req->adrs : register address
 *       req->ptr : pointer to data
 *  \retval 0: Success, -1 : Failure 
 */
static int DWC_ETH_QOS_RD_WR_REG(struct ifr_data_struct *req)
{
    unsigned int ret = -1;
    unsigned int data_req_var = 0;

    if(copy_from_user(&data_req_var, req->ptr, sizeof(unsigned int)))
	printk("copy_from_user error : ifr data structure\n");
	
    switch(req->flags)
    {
        case TC9560_REG_RD: /* TC9560 Register Read */
				tc9560_read(req->adrs,&data_req_var);
				if(copy_to_user(req->ptr,&data_req_var, sizeof(unsigned int)))
					printk("copy_to_user error: ifr data structure\n");
                ret = 0;
                break;
        case TC9560_REG_WR: /* TC9560 Register Write */
				tc9560_write(req->adrs, data_req_var);
                ret = 0;
                break;
        default:
                ret = -1;
                break;
    }
    return ret;
}


static void config_rx_outer_vlan_stripping(u32 cmd)
{
	MAC_VLANTR_EVLS_UdfWr(cmd);
}
/*!
 * \brief This function confiures the TDM path for TDM-Ethernet data transfer. 
 * \param[in] pdata : pointer to private data structure.
 * \param[in] req : pointer to ioctl structure.
 *
 * \retval 0: Success, -1 : Failure
 **/
static int TC9560_TDM_Config(struct ifr_data_struct *req)
{    
	struct TC9560_TDM_Config *tc9560_tdm_cfg, tc9560_tdm_cnfg_data;
	u32 reg_val;
	unsigned char* ptr;
	unsigned int int_mask_val;
	int i;
	
	if(copy_from_user(&tc9560_tdm_cnfg_data, req->ptr, sizeof(struct TC9560_TDM_Config)))
		printk("copy_from_user error: ifr data structure\n");

	tc9560_tdm_cfg = (struct TC9560_TDM_Config *)(&tc9560_tdm_cnfg_data);

	//Assert TDM reset	
	tc9560_read(0x1008, &reg_val);
	reg_val |= 0x1<<6;
	tc9560_write(0x1008, reg_val);

	/* Stop TDM */
	if(tc9560_tdm_cfg->tdm_start == 0) {
     		
		DBGPR("Disabled TDM path\n"); 

		//Disable TDM clock
		tc9560_read(0x1004, &reg_val);
		reg_val &= ~(0x1<<6);
		tc9560_write(0x1004, reg_val);
		
		/* Disable TDM interrupt in INTC Module */
#ifdef TC9560_INT_INTX
		TC9560_INTC_INTINTXMASK1_RgRd(int_mask_val);
		int_mask_val |= (0x7F<<22);	//Disable all TDM interrupts 
		TC9560_INTC_INTINTXMASK1_RgWr(int_mask_val);
#else
		TC9560_INTC_INTMCUMASK1_RgRd(int_mask_val);
		int_mask_val |= (0x7F<<22);	//Disable all TDM interrupts 
		TC9560_INTC_INTMCUMASK1_RgWr(int_mask_val);
#endif
		return 0;
	}

   	/* Enable TDM interrupt in INTC Module */
#ifdef TC9560_INT_INTX
	TC9560_INTC_INTINTXMASK1_RgRd(int_mask_val);
	int_mask_val &= ~(0x7F<<22); //Enable all interrupts
	TC9560_INTC_INTINTXMASK1_RgWr(int_mask_val);
#else
	TC9560_INTC_INTMCUMASK1_RgRd(int_mask_val);
	int_mask_val &= ~(0x7F<<22); //Enable all interrupts
	TC9560_INTC_INTMCUMASK1_RgWr(int_mask_val);
#endif

	//Enable TDM clock
	tc9560_read(0x1004, &reg_val);
	reg_val |= 0x1<<6;
	tc9560_write(0x1004, reg_val);
	
	//Deassert TDM reset	
	tc9560_read(0x1008, &reg_val);
	reg_val &= ~(0x1<<6);
	tc9560_write(0x1008, reg_val);

	/* Start TDM */
	DBGPR("TDM Path Configuration\n");
	DBGPR("    Sample rate = %d\n", tc9560_tdm_cfg->sample_rate);
	DBGPR("    No of channels = %d\n", tc9560_tdm_cfg->channels);
	DBGPR("    Mode selected  = %d\n", tc9560_tdm_cfg->mode_sel);
	DBGPR("    Protocol selected = %d\n", tc9560_tdm_cfg->protocol);
	DBGPR("    Class priority  = %d\n", tc9560_tdm_cfg->a_priority);
	DBGPR("    Direction selected = %d\n", tc9560_tdm_cfg->direction);
	DBGPR("    Class vid = %d\n", tc9560_tdm_cfg->a_vid);

	switch(tc9560_tdm_cfg->sample_rate){
		case 48000:
		{
			if(tc9560_tdm_cfg->channels == 2)
			{
				tc9560_write(0x44a0, 0x00000C00);//T0EVB_DDANRatio (N value)
				tc9560_write(0x44e0, 0x0000060C);//T0EVB_DDACtrl2  (PLL output divider)
				tc9560_write(0x44e8, 0x0000f300);//DDA_PLL_Ctrl2  (BCLK and MCLK divider)
			}
			else if (tc9560_tdm_cfg->channels == 4)
			{
				tc9560_write(0x44a0, 0x00001800);//T0EVB_DDANRatio (N value)
				tc9560_write(0x44e0, 0x0000070C);//T0EVB_DDACtrl2  (PLL output divider)
				tc9560_write(0x44e8, 0x00007300);//DDA_PLL_Ctrl2  (BCLK and MCLK divider)
			}
			else if (tc9560_tdm_cfg->channels == 8)
			{
				tc9560_write(0x44a0, 0x00003000);//T0EVB_DDANRatio (N value)
				tc9560_write(0x44e0, 0x0000080C);//T0EVB_DDACtrl2  (PLL output divider)
				tc9560_write(0x44e8, 0x00003300);//DDA_PLL_Ctrl2  (BCLK and MCLK divider)
			}
			else
			{
				printk("Can't configure 6 channels mode\n");
			}
			tc9560_write(0x449c, 0x02dc6c08);//TDM DDA Control (M value) 
			tc9560_write(0x44e4, 0x01fff700);//DDA_PLL_Ctrl1
			tc9560_write(0x44ec, 0x00000000);//DDA_PLL_UPDT
			tc9560_tdm_cfg->fdf = 2;
		}
       		break;
		default:
			break;	
	}

	/*Clock & Reset*/
	tc9560_write(0x0010, 0x3030008);//TC9560 eMAC DIV

	/*EMAC RX*/
	/* TDM Source MAC ID */
	ptr = tc9560_tdm_cfg->TDM_SRC_ID;	
	DBGPR("SRC: \n");
	for(i=0;i<sizeof(tc9560_tdm_cfg->TDM_SRC_ID); i++)
		DBGPR("%x : %x \n",ptr[i], tc9560_tdm_cfg->TDM_SRC_ID[i]);

	reg_val = (ptr[3] << 24) | (ptr[2] << 16) | (ptr[1] << 8) | ptr[0];
	tc9560_write(0x3010, reg_val);//TDM Header Ethernet MAC Source Low Address Register 
	reg_val = (ptr[5] << 8) | (ptr[4] << 0);
	tc9560_write(0x3014, reg_val);//TDM Header Ethernet MAC Source High Address Register 

	tc9560_write(0xab98, 0x00003D08);//MAC_PPS0_Width : 125 us
	tc9560_write(0xab70, 0x00000212);//MAC_PPS_Control : Set for only PPS0 output
	
	 
	/*TDM*/
	if(tc9560_tdm_cfg->direction == TC9560_TDM_IN)
	{
		tc9560_write(0x4414, 0x08100058);//TDM conf0 

		tc9560_read(0x3000, &reg_val);
		reg_val &= ~(0x7<<2); // Making bits related to priority as Zeros before updating
		reg_val |= (tc9560_tdm_cfg->a_priority&0x7)<<2;
		reg_val |= (tc9560_tdm_cfg->fdf&0xFF)<<8;
		tc9560_write(0x3000, reg_val);//TDM Control Register

   		/* config stream id */
        	ptr = tc9560_tdm_cfg->TDM_STREAM_ID;
		DBGPR("Stream: \n");
		for(i=0;i<sizeof(tc9560_tdm_cfg->TDM_STREAM_ID); i++)
			DBGPR("%x : %x \n",ptr[i], tc9560_tdm_cfg->TDM_STREAM_ID[i]);
	
        	reg_val = (ptr[3] << 24) | (ptr[2] << 16) | (ptr[1] << 8) | ptr[0];
		tc9560_write(0x4418, reg_val);//TDM Stream ID Low 
        	reg_val = (ptr[7] << 24) | (ptr[6] << 16) | (ptr[5] << 8) | ptr[4];
		tc9560_write(0x441c, reg_val);//TDM Stream ID Hi 

		/* TDM Destination MAC ID */
		ptr = tc9560_tdm_cfg->TDM_DST_ID;	
		DBGPR("DST: \n");
		for(i=0;i<sizeof(tc9560_tdm_cfg->TDM_DST_ID); i++)
			DBGPR("%x : %x \n",ptr[i], tc9560_tdm_cfg->TDM_DST_ID[i]);
	
		reg_val = (ptr[3] << 24) | (ptr[2] << 16) | (ptr[1] << 8) | ptr[0];
		tc9560_write(0x4420, reg_val);//TDM Stream ID Low 
		reg_val = (ptr[5] << 8) | (ptr[4] << 0);
		tc9560_write(0x4424, reg_val);//TDM Stream ID Low 
	
		reg_val = 0x40005800;
		reg_val |= (tc9560_tdm_cfg->channels & 0xF)<<16;
		reg_val |= (tc9560_tdm_cfg->protocol & 0x1)<<15;
		tc9560_write(0x4410, reg_val);//TDM Control Register

		if(tc9560_tdm_cfg->channels == 2)
		{
			reg_val = 0x2003072C; // no.of channels as 2
		}
		else if (tc9560_tdm_cfg->channels == 4)
		{
			reg_val = 0x20070F2C; // no.of channels as 4
		}
		else if (tc9560_tdm_cfg->channels == 8)
		{
			reg_val = 0x200F1F2C; // no.of channels as 8
		}
		reg_val |= (tc9560_tdm_cfg->mode_sel & 0x1)<<31;
		tc9560_write(0x4400, reg_val);//TDM Control Register
	
		tc9560_write(0x100C, 0x00002600);//Pin Mux control 
		DBGPR("TDM IN configured in I2S mode\n");
	
#ifndef TC9560_DRV_TEST_LOOPBACK
	} else {
#endif
		/* Disable VLAN striping */
        config_rx_outer_vlan_stripping(DWC_ETH_QOS_RX_NO_VLAN_STRIP);

		tc9560_write(0x100C, 0x00002600);//Pin Mux control 
		if(tc9560_tdm_cfg->channels == 2)
		{
			tc9560_write(0x4490, 0x01F00213);//TDM Output Control Register
		}
		else if (tc9560_tdm_cfg->channels == 4)
		{
			tc9560_write(0x4490, 0x01F00413);//TDM Output Control Register
		}
		else if (tc9560_tdm_cfg->channels == 8)
		{
			tc9560_write(0x4490, 0x01F00813);//TDM Output Control Register
		}
		tc9560_write(0x44b8, 0x00000000);//Pin Mux control 

		/* config stream id */
		ptr = tc9560_tdm_cfg->TDM_STREAM_ID;
		DBGPR("Stream: \n");
		for(i=0;i<sizeof(tc9560_tdm_cfg->TDM_STREAM_ID); i++)
			DBGPR("%x : %x \n",ptr[i], tc9560_tdm_cfg->TDM_STREAM_ID[i]);

		reg_val = (ptr[3] << 24) | (ptr[2] << 16) | (ptr[1] << 8) | ptr[0];
		tc9560_write(0x3084, reg_val);//TDM Stream ID Hi
		reg_val = (ptr[7] << 24) | (ptr[6] << 16) | (ptr[5] << 8) | ptr[4];
		tc9560_write(0x3088, reg_val);//TDM Stream ID Low

		tc9560_write(0x44CC, 0x00CC0133);//TDM buffer near underflow threshold register 
		tc9560_write(0x44D0, 0x02CC0333);//TDM buffer near overflow threshold register 

		if(tc9560_tdm_cfg->channels == 2)
		{
			reg_val = 0x0003072C; // no.of channels as 2
		}
		else if (tc9560_tdm_cfg->channels == 4)
		{
			reg_val = 0x00070F2C; // no.of channels as 4
		}
		else if (tc9560_tdm_cfg->channels == 8)
		{
			reg_val = 0x000F1F2C; // no.of channels as 8
		}
		reg_val |= (tc9560_tdm_cfg->mode_sel & 0x1)<<31;
		tc9560_write(0x4400, reg_val);//TDM Control Register
		DBGPR("TDM OUT configured in I2S mode\n");
	}

	if(copy_to_user(req->ptr, &tc9560_tdm_cnfg_data, sizeof(struct TC9560_TDM_Config)))
		printk("copy_to_user error: ifr data structure\n");
	return 0;
}

/*!
 *  \brief Driver IOCTL routine
 *  \details This function is invoked by main ioctl function when
 *  users request to configure various device features like,
 *  PMT module, TX and RX PBL, TX and RX FIFO threshold level,
 *  TX and RX OSF mode, SA insert/replacement, L2/L3/L4 and
 *  VLAN filtering, AVB/DCB algorithm etc.
 *  \param[in] pdata – pointer to private data structure.
 *  \param[in] req – pointer to ioctl structure.
 *  \return int
 *  \retval 0 - success
 *  \retval negative - failure
 **/
static int DWC_ETH_QOS_handle_prv_ioctl(struct net_device *net, struct ifr_data_struct *req)
{
		struct usbnet *dev = netdev_priv(net);
		struct tc9560_data *priv = (struct tc9560_data *)dev->data[0]; 
        unsigned int chInx = req->chInx;
        int ret = 0;

        if (chInx > TC9560_QUEUE_CNT) {
                printk(KERN_ALERT "Queue number %d is invalid\n" \
                                "Hardware has only %d Tx/Rx Queues\n",
                                chInx, TC9560_QUEUE_CNT);
                ret = DWC_ETH_QOS_NO_HW_SUPPORT;
		req->connected_speed = nic_speed;
                return ret;
        }

        switch (req->cmd) {
        case DWC_ETH_QOS_AVB_ALGORITHM:
		ret = DWC_ETH_QOS_program_avb_algorithm(req);

                break;
	
	case DWC_ETH_QOS_GET_CONNECTED_SPEED:
		/*ret = tc9560_read(MAC_OFFSET, &mac_config);
		if((mac_config & 0xC000) == 0)
			req->connected_speed = SPEED_1000;
		else if ((mac_config & 0xC000) == 0xC000)
			req->connected_speed = SPEED_100;
		else if ((mac_config & 0xC000) == 0x8000)
			req->connected_speed = SPEED_10;*/
		req->connected_speed = priv->Speed;
		break;

	case DWC_ETH_QOS_GET_RX_QCNT:
		req->chInx = TC9560_RX_QUEUE_CNT;
		break;

	case DWC_ETH_QOS_GET_TX_QCNT:
		req->chInx = TC9560_TX_QUEUE_CNT;
		break;
	
	case DWC_ETH_QOS_L2_DA_FILTERING_CMD:
		ret = DWC_ETH_QOS_confing_l2_da_filter(net, req);
		break;

	case DWC_ETH_QOS_PHY_LOOPBACK_MODE_CMD:
       ret = DWC_ETH_QOS_config_phy_loopback_mode(req->flags);
        if (ret == 0)
                ret = DWC_ETH_QOS_CONFIG_SUCCESS;
        else
                ret = DWC_ETH_QOS_CONFIG_FAIL;
        break;

	case DWC_WRAP_TS_FEATURE:
		tc9560_wrap_ts_ignore_config(req->flags);
        printk(KERN_ALERT "TC9560 Wrapper TS Feature Value: %d\n", req->flags);
        break;

    case TC9560_DWC_REG_RD_WR_CMD:
        ret = DWC_ETH_QOS_RD_WR_REG((void*)req);
        break;

	case TC9560_DWC_TDM_CONFIG_CMD:
		ret = TC9560_TDM_Config((void*)req);
		break;

	}
	return ret;
}

/*!
 * \details This function gets the PHC index
 * \param[in] dev <96> pointer to net device structure.
 * \param[in] ethtool_ts_info <96> pointer to ts info structure.
 *
 * \return int
 *
 * \retval +ve(>0) on success, 0 if that string is not
 * \defined and -ve on failure.
 */
static int DWC_ETH_QOS_get_ts_info(struct net_device *dev,
                           struct ethtool_ts_info *info)
{
    DBGPR("-->DWC_ETH_QOS_get_ts_info\n");
    info->phc_index = tc9560_phc_index(pdata_phc);
    DBGPR("PHC index = %d\n", info->phc_index);
    DBGPR("<--DWC_ETH_QOS_get_ts_info\n");
    return 0;
}


#ifndef TC9560_DRV_TEST_LOOPBACK
/**
 * Function used to detect the cable plugging and unplugging.
 * This function gets scheduled once in every second and polls
 * the PHY register for network cable plug/unplug. Once the 
 * connection is back the GMAC device is configured as per
 * new Duplex mode and Speed of the connection.
 * @param[in] u32 type but is not used currently. 
 * \return returns void.
 * \note This function is tightly coupled with Linux 2.6.xx.
 * \callgraph
 */
static void synopGMAC_linux_cable_unplug_function(void)
{
	s32 status;
	u16 data;
	u32 new_speed, new_duplex, new_linkstate;
	struct usbnet *dev = (struct usbnet *)synopGMAC_cable_unplug_timer.data;
	struct tc9560_data *priv = (struct tc9560_data *)dev->data[0]; 
	int new_state = 0;

	status = synopGMAC_read_phy_reg(dev, priv->phy_id, PHY_SPECIFIC_STATUS_REG, &data);

	/* Get New Phy duplex mode */
	new_duplex = (data & Mii_phy_status_full_duplex) ? FULLDUPLEX:HALFDUPLEX;

	/* Get New Phy speed */
	if(data & Mii_phy_status_speed_1000)
		new_speed = SPEED_1000;
	else if(data & Mii_phy_status_speed_100)
		new_speed = SPEED_100;
	else
		new_speed = SPEED_10;
	
	/* Get new link status */
	new_linkstate = (data & Mii_phy_status_link_up) ? LINKUP : LINKDOWN;

	if (new_linkstate == LINKUP) {
		/* Link is active */
		/* Check if duplex mode is changed */
		if (new_duplex != priv->DuplexMode) {
			new_state = 1;
			if (new_duplex)
				synopGMAC_set_full_duplex(dev);	
			else
				synopGMAC_set_half_duplex(dev);	
			priv->DuplexMode = new_duplex;
		}

		/* Check if speed is changed */
		if (new_speed != priv->Speed) {
			new_state = 1;
			priv->Speed = new_speed;
			synopGMAC_set_speed(dev, priv->Speed);
		}

		/* Restart if previous link was down. */
		if(new_linkstate != priv->LinkState) {
			new_state = 1;
			priv->LinkState = new_linkstate;
			netif_carrier_on(dev->net);
			usbnet_defer_kevent(dev,EVENT_LINK_RESET);
		}

	} else {
		if(new_linkstate != priv->LinkState) {
			/* Link is down */
			new_state = 1;
			priv->DuplexMode = -1;
		    priv->Speed = 0;
		    priv->LoopBackMode = 0; 
			priv->LinkState = new_linkstate;
		    netif_carrier_off(dev->net);
	    }
    }

	if (new_state){
		if (priv->LinkState == LINKUP) {
			printk("TC9560 HSIC Link is Up - %s/%s\n",
					((priv->Speed == SPEED_10) ? "10Mbps": ((priv->Speed == SPEED_100) ? "100Mbps": "1Gbps")),
					(priv->DuplexMode == FULLDUPLEX) ? "Full Duplex": "Half Duplex");
		} else {
			printk("TC9560 HSIC Link is Down\n");
	    }
    }

	schedule_delayed_work(&task, msecs_to_jiffies(TC9560_PHY_DETECT_INTERVAL));
}
#endif

static void tc9560_status(struct usbnet *dev, struct urb *urb)
{
	struct tc9560_int_data *event;
	int link;

	if (urb->actual_length < 8)
		return;

	event = urb->transfer_buffer;
	link = event->link & TC9560_INT_PPLS_LINK;

	if (netif_carrier_ok(dev->net) != link) {
		if (link)
			usbnet_defer_kevent(dev, EVENT_LINK_RESET);
		else
			netif_carrier_off(dev->net);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 34)
		netdev_info(dev->net, "TC9560 - Link status is: %d\n",
				link);
#else
		devinfo(dev, "TC9560 - Link status is: %d\n", link);
#endif
	}
}

static int tc9560_mdio_read(struct net_device *netdev, int phy_id, int loc)
{
	struct usbnet *dev = netdev_priv(netdev);
	u32 ret;
	u16 reg_val;

	ret = synopGMAC_read_phy_reg(dev, phy_id, loc, &reg_val);
	CHECK(ret, "synopGMAC_read_phy_reg");

	return reg_val;
}

static void tc9560_mdio_write(struct net_device *netdev, int phy_id, int loc,
		int val)
{
	struct usbnet *dev = netdev_priv(netdev);

	DBGPR ("__FUNCTION__ = %s\n", __FUNCTION__);	
	synopGMAC_write_phy_reg(dev, phy_id, loc, (u16)val);
}

static int tc9560_suspend(struct usb_interface *intf,
#if LINUX_VERSION_CODE > KERNEL_VERSION(2, 6, 10)
		pm_message_t message)
#else
u32 message)
#endif
{
	usbnet_suspend(intf, message);
	return 0;
}

static int tc9560_resume(struct usb_interface *intf)
{
	struct usbnet *dev = usb_get_intfdata(intf);
	u32 ret; 


	/* Power up ethernet PHY */
	ret = synopGMAC_check_phy_init(dev, 0);   
	if (ret < 0)
	{
		return ret;   
	}         

	return usbnet_resume(intf);
}


static void tc9560_get_drvinfo(struct net_device *net,
		struct ethtool_drvinfo *info)
{
	/* Inherit standard device info */
	DBGPR("-->tc9560_get_drvinfo\n");
	usbnet_get_drvinfo(net, info);
	strcpy(info->driver, DEV_NAME);
    strcpy(info->version, DRV_VERSION);
    strcpy(info->fw_version, FW_VERSION);
	info->eedump_len = 0x3e;
}

static int tc9560_get_settings(struct net_device *net, struct ethtool_cmd *cmd)
{
	struct usbnet *dev = netdev_priv(net);
	DBGPR("-->tc9560_get_settings\n");
	return mii_ethtool_gset(&dev->mii, cmd);
}

static int tc9560_set_settings(struct net_device *net, struct ethtool_cmd *cmd)
{
	struct usbnet *dev = netdev_priv(net);
	DBGPR("-->tc9560_set_settings\n");
	return mii_ethtool_sset(&dev->mii, cmd);
}

/**********************************************************/
#if (IOCTL_DEFINE == 1) 
static void tc9560_write_bulk_callback(struct urb *urb) 
{
        /* sync/async unlink faults aren't errors */
      struct tc9560_urb *tc9560_urb_call;

      tc9560_urb_call = (struct tc9560_urb *)urb->context;
        
      if (urb->status && !(urb->status == -ENOENT || urb->status == -ECONNRESET || urb->status == -ESHUTDOWN)) 
      {
        printk("%s - nonzero write bulk status received: %d",__func__, urb->status);
      }        
      up(&tc9560_urb_call->tc9560_sem);
}
#endif

static int tc9560_ioctl(struct net_device *net, struct ifreq *ifr, int cmd)
{
	struct usbnet *dev = netdev_priv(net);
	struct tc9560_data *priv = NULL;
	s32 retval = 0, phc_index = 0;

	struct ifr_data_struct *req , reg_rw, ifr_data_req;
#if (IOCTL_DEFINE == 1)
  struct ifr_data_struct_1722 pkt_1722, *pkt_1722_userspace;
#endif

	if(ifr == NULL)
	{
		return -1;
	}
	
	priv = (struct tc9560_data *)dev->data[0];

	switch(cmd)
	{
		case DWC_ETH_QOS_PRV_IOCTL:
			retval= copy_from_user(&ifr_data_req, ifr->ifr_data, sizeof(struct ifr_data_struct));
			if(retval)
			{
				printk("copy_from_user error: ifr structure\n");
				retval = -EINVAL;
			}
			req = (struct ifr_data_struct *)(&ifr_data_req);

			DWC_ETH_QOS_handle_prv_ioctl(net, req);
			req->command_error = retval;
			if(copy_to_user( ifr->ifr_data, &ifr_data_req, sizeof(struct ifr_data_struct)))
			printk("copy_to_user error: ifr structure\n");
			break;
		/* IOCTL to read GMAC registers */
		case IOCTL_READ_REGISTER:

			retval = copy_from_user(&reg_rw, ifr->ifr_data, sizeof(struct ifr_data_struct));
			if(retval)
			{
				printk("copy_from_user error: ifr structure\n");
				retval = -EINVAL;
			}
			retval = tc9560_reg_read(dev, reg_rw.addr, &reg_rw.data);

			if(copy_to_user(ifr->ifr_data, &reg_rw, sizeof(struct ifr_data_struct)))
				printk("ERROR : retval = %d\n",retval);

			CHECK(retval, "IOCTL_READ_REGISTER: tc9560_reg_read failed");
			break;

		/* IOCTL to write GMAC registers */
		case IOCTL_WRITE_REGISTER:
			retval = copy_from_user(&reg_rw, ifr->ifr_data, sizeof(struct ifr_data_struct));

			if(retval)
			{
				printk("copy_from_user error: ifr structure\n");
				retval = -EINVAL;
			}
			retval = tc9560_reg_write(dev,reg_rw.addr,reg_rw.data);

			CHECK(retval, "IOCTL_WRITE_REGISTER: tc9560_reg_write failed");
			break;

		case SIOCSHWTSTAMP:
			retval = tc9560_ptp_hwtstamp_ioctl(net, ifr,  cmd);
			CHECK(retval, "SIOCSHWTSTAMP: tc9560_ptp_hwtstamp_ioctl failed");
			break;

		case SIOCETHTOOL:
			if(copy_to_user(ifr->ifr_data, &phc_index, sizeof(phc_index)))
				printk("IOCTL(SIOCETHTOOLTC9560) copy_to_user failed\n");
			break;
#if (IOCTL_DEFINE == 1)
    case IOCTL_AVB_A_EP2_TX:/* IOCTL_EP2_TX: */
      {
      	priv = (struct tc9560_data *)dev->data[0];
	pkt_1722_userspace = (struct ifr_data_struct_1722 *)ifr->ifr_data;        
	retval = copy_from_user(&pkt_1722, pkt_1722_userspace, sizeof(struct ifr_data_struct_1722));
	if(retval)
	{
      		printk("copy_from_user error failed: pkt_1722\n");
          	retval = -EINVAL;
	}
        if (down_interruptible(&tc9560_urb_context.tc9560_sem)) 
        {
        	printk("\n\rSleep Interrupted:%s:%d\n\r",__func__,__LINE__);
	        retval = -ERESTARTSYS;
	        goto exit;
        }
    
        usb_init_urb(tc9560_urb_alloc[tc9560_urb_context.tc9560_i]); 
        //retval = copy_from_user(dma_buf[tc9560_urb_context.tc9560_i], (uint8_t *)pkt_1722_userspace->data,ALLOC_SIZE);
	memcpy(dma_buf[tc9560_urb_context.tc9560_i], pkt_1722.data, pkt_1722.pkt_len);
        usb_fill_bulk_urb(tc9560_urb_alloc[tc9560_urb_context.tc9560_i],
                        dev->udev,
                        priv->avb_a_out,
                        (void*) dma_buf[tc9560_urb_context.tc9560_i],
                        pkt_1722.pkt_len,//ALLOC_SIZE,
                        tc9560_write_bulk_callback,&tc9560_urb_context);
    
        retval = usb_submit_urb(tc9560_urb_alloc[tc9560_urb_context.tc9560_i], GFP_ATOMIC);
        if (retval) 
        {
          printk(KERN_INFO "\n\r%s - failed submitting write urb, error %d\n\r",__func__, retval);
          goto error;
        }
        tc9560_urb_context.tc9560_i++;
        if(tc9560_urb_context.tc9560_i == 20)
        tc9560_urb_context.tc9560_i = 0;
        return retval;

error:
        up(&tc9560_urb_context.tc9560_sem);
exit:        
        return retval;
      }      
      break;
#endif
		default:
			return  generic_mii_ioctl(&dev->mii, if_mii(ifr), cmd, NULL);
	}
	return retval;

}

/*!
* \brief API to configure the multicast address in device.
*
* \details This function collects all the multicast addresse
* and updates the device.
*
* \param[in] dev - pointer to net_device structure.
*
* \retval 0 if perfect filtering is seleted & 1 if hash
* filtering is seleted.
*/
static int DWC_ETH_QOS_prepare_mc_list(struct net_device *net)
{
	struct usbnet *dev = netdev_priv(net);
	struct tc9560_data *priv = (struct tc9560_data *)dev->data[0]; 
	u32 mc_filter[DWC_ETH_QOS_HTR_CNT];
	struct netdev_hw_addr *ha = NULL;
	int crc32_val = 0;
	int ret = 0, i = 1;

	DBGPR("-->DWC_ETH_QOS_prepare_mc_list\n");

	if (priv->l2_filtering_mode) {
		DBGPR("select HASH FILTERING for mc addresses: mc_count = %d\n",
				netdev_mc_count(net));
		ret = 1;
		memset(mc_filter, 0, sizeof(mc_filter));

		netdev_for_each_mc_addr(ha, net) {
			DBGPR("mc addr[%d] = %x:%x:%x:%x:%x:%x\n",i++,
					ha->addr[0], ha->addr[1], ha->addr[2],
					ha->addr[3], ha->addr[4], ha->addr[5]);
			/* The upper 6 bits of the calculated CRC are used to
			 * index the content of the Hash Table Reg 0 and 1.
			 * */
			crc32_val =
				(bitrev32(~crc32_le(~0, ha->addr, 6)) >> 26);
			/* The most significant bit determines the register
			 * to use (Hash Table Reg X, X = 0 and 1) while the
			 * other 5(0x1F) bits determines the bit within the
			 * selected register
			 * */
			mc_filter[crc32_val >> 5] |= (1 << (crc32_val & 0x1F));
		}
		for (i = 0; i < DWC_ETH_QOS_HTR_CNT; i++)
			update_hash_table_reg(i, mc_filter[i]);

	} else {
		DBGPR("select PERFECT FILTERING for mc addresses, mc_count = %d, max_addr_reg_cnt = %d\n",
				netdev_mc_count(net), TC9560_MAX_ADDR_REG_CNT);

		netdev_for_each_mc_addr(ha, net) {
			DBGPR("mc addr[%d] = %x:%x:%x:%x:%x:%x\n", i,
					ha->addr[0], ha->addr[1], ha->addr[2],
					ha->addr[3], ha->addr[4], ha->addr[5]);
			if (i < 30) /* MAC Address 0,1,2 are reserved so we have to update from 3 to 31 address i.e 29 address we can configure*/
				update_mac_addr3_31_low_high_reg(i, ha->addr);
			else
				;
			i++;
		}
	}

	DBGPR("<--DWC_ETH_QOS_prepare_mc_list\n");

	return ret;
}

/*
* \brief API to configure the unicast address in device.
*
* \details This function collects all the unicast addresses
* and updates the device.
*
* \param[in] net - pointer to net_device structure.
*
* \retval 0 if perfect filtering is seleted  & 1 if hash
* filtering is seleted.
*/
static int DWC_ETH_QOS_prepare_uc_list(struct net_device *net)
{
	struct usbnet *dev = netdev_priv(net);
	struct tc9560_data *priv = (struct tc9560_data *)dev->data[0]; 
	u32 uc_filter[DWC_ETH_QOS_HTR_CNT];
	struct netdev_hw_addr *ha = NULL;
	int crc32_val = 0;
	int ret = 0, i = 1;

	DBGPR("-->DWC_ETH_QOS_prepare_uc_list\n");

	if (priv->l2_filtering_mode) {
		DBGPR("select HASH FILTERING for uc addresses: uc_count = %d\n",
				netdev_uc_count(net));
		ret = 1;
		memset(uc_filter, 0, sizeof(uc_filter));

		netdev_for_each_uc_addr(ha, net) {
			DBGPR("uc addr[%d] = %#x:%#x:%#x:%#x:%#x:%#x\n",i++,
					ha->addr[0], ha->addr[1], ha->addr[2],
					ha->addr[3], ha->addr[4], ha->addr[5]);
			crc32_val =
				(bitrev32(~crc32_le(~0, ha->addr, 6)) >> 26);
			uc_filter[crc32_val >> 5] |= (1 << (crc32_val & 0x1F));
		}
		

		// configure hash value of real/default interface also 
		DBGPR("real/default dev_addr = %x:%x:%x:%x:%x:%x\n",
				net->dev_addr[0], net->dev_addr[1], net->dev_addr[2],
				net->dev_addr[3], net->dev_addr[4], net->dev_addr[5]);
				
			crc32_val =
				(bitrev32(~crc32_le(~0, net->dev_addr, 6)) >> 26);
			uc_filter[crc32_val >> 5] |= (1 << (crc32_val & 0x1F));

		for (i = 0; i < DWC_ETH_QOS_HTR_CNT; i++)
			update_hash_table_reg(i, uc_filter[i]);

	} else {
		DBGPR("select PERFECT FILTERING for uc addresses: uc_count = %d\n",
				netdev_uc_count(net));

		netdev_for_each_uc_addr(ha, net) {
			DBGPR("uc addr[%d] = %x:%x:%x:%x:%x:%x\n", i,
					ha->addr[0], ha->addr[1], ha->addr[2],
					ha->addr[3], ha->addr[4], ha->addr[5]);
			if (i < 30) /* MAC Address 0,1,2 are reserved so we have to update from 3 to 31 address i.e 29 address we can configure*/
				update_mac_addr3_31_low_high_reg(i, ha->addr);
			else
				;
			i++;
		}
	}

	DBGPR("<--DWC_ETH_QOS_prepare_uc_list\n");

	return ret;
}


static struct ethtool_ops tc9560_ethtool_ops = {
	.get_drvinfo		= tc9560_get_drvinfo,
	.get_link		    = ethtool_op_get_link,
	.get_msglevel		= usbnet_get_msglevel,
	.set_msglevel		= usbnet_set_msglevel,
	.get_settings		= tc9560_get_settings,
	.set_settings		= tc9560_set_settings,
	.get_ts_info 		= DWC_ETH_QOS_get_ts_info,
};

/*!
* \brief API to set the device receive mode
*
* \details The set_multicast_list function is called when the multicast list
* for the device changes and when the flags change.
*
* \param[in] net - pointer to net_device structure.
*
* \return void
*/

static void tc9560_set_rx_mode(struct net_device *net)
{
	unsigned char pr_mode = 0;
	unsigned char huc_mode = 0;
	unsigned char hmc_mode = 0;
	unsigned char pm_mode = 0;
	unsigned char hpf_mode = 0;
	struct usbnet *dev = netdev_priv(net);
	struct tc9560_data *priv = (struct tc9560_data *)dev->data[0];
	int mode, i;

	DBGPR("-->DWC_ETH_QOS_set_rx_mode \n");
  
	if (net->flags & IFF_PROMISC) {
		DBGPR("PROMISCUOUS MODE (Accept all packets irrespective of DA)\n");
		pr_mode = 1;
	} 
	else if ((net->flags & IFF_ALLMULTI) ||
			(netdev_mc_count(net) > TC9560_MAX_HASH_TABLE_SIZE)) {
		DBGPR("pass all multicast pkt\n");
		pm_mode = 1;
		for (i = 0; i < DWC_ETH_QOS_HTR_CNT; i++)
			update_hash_table_reg(i, 0xffffffff);
	} 
	else if (!netdev_mc_empty(net)) {
		DBGPR("pass list of multicast pkt\n");
		if ((netdev_mc_count(net) > (TC9560_MAX_ADDR_REG_CNT - 1)) &&
			(!TC9560_MAX_HASH_TABLE_SIZE)) {
		DBGPR("PROMISCUOUS MODE (multicast)\n");
			// switch to PROMISCUOUS mode 
			pr_mode = 1;
		} else {
			mode = DWC_ETH_QOS_prepare_mc_list(net);
			if (mode) {
		DBGPR("Hash filtering for multicast\n");
				// Hash filtering for multicast 
				hmc_mode = 1;
			} else {
		DBGPR("Perfect filtering for multicast\n");
				// Perfect filtering for multicast 
				hmc_mode = 0;
				hpf_mode = 1;
			}
		}
	}
	// Handle multiple unicast addresses 
	if ((netdev_uc_count(net) > (TC9560_MAX_ADDR_REG_CNT - 1)) &&
			(!TC9560_MAX_HASH_TABLE_SIZE)) {
		DBGPR("PROMISCUOUS MODE (unicast)\n");
		// switch to PROMISCUOUS mode 
		pr_mode = 1;
	} 
	else if (!netdev_uc_empty(net)) {
		DBGPR("pass all unicast pkt\n");
		mode = DWC_ETH_QOS_prepare_uc_list(net);
		if (mode) {
		DBGPR("Hash filtering for unicast\n");
			// Hash filtering for unicast 
			huc_mode = 1;
		} else {
		DBGPR("Perfect filtering for unicast\n");
			// Perfect filtering for unicast 
			huc_mode = 0;
			hpf_mode = 1;
		}
	}
	config_mac_pkt_filter_reg(priv, pr_mode, huc_mode, hmc_mode, pm_mode, hpf_mode);
	DBGPR("<--DWC_ETH_QOS_set_rx_mode\n");
}

static int tc9560_change_mtu(struct net_device *net, int new_mtu)
{
	struct usbnet *dev = netdev_priv(net);

	if (new_mtu <= 0 || new_mtu > 2000)
		return -EINVAL;

	net->mtu = new_mtu;
	dev->hard_mtu = net->mtu + net->hard_header_len;

	return 0;
}

static int tc9560_set_mac_addr(struct net_device *net, void *p)
{
	struct usbnet *dev = netdev_priv(net);
	struct sockaddr *addr = p;

	if (netif_running(net))
		return -EBUSY;
	if (!is_valid_ether_addr(addr->sa_data))
		return -EADDRNOTAVAIL;

	memcpy(net->dev_addr, addr->sa_data, ETH_ALEN);

	/* Set the MAC address */
	synopGMAC_set_mac_addr(dev, MAC_MA2HR_RgOffAddr, MAC_MA2LR_RgOffAddr, addr->sa_data); 

	TR("%s called \n",__FUNCTION__);
	return 0;
}

/* For MQS */
static u16 tc9560_select_queue(struct net_device *net, struct sk_buff *skb,
                            void *accel_priv, select_queue_fallback_t fallback)
{
	u16 *skb_data_ptr = NULL;
	u16 eth_avb_ptp,eth_avb;
	u16  eth_avb_class;
	int ret = 0;

	skb_data_ptr = (u16 *)skb->data;
	eth_avb_ptp = *(skb_data_ptr + 6);
	eth_avb_ptp = cpu_to_be16(eth_avb_ptp);

	eth_avb = *(skb_data_ptr + 8);
	eth_avb = cpu_to_be16(eth_avb);
	eth_avb_class = *(skb_data_ptr + 7);
	eth_avb_class = cpu_to_be16(eth_avb_class);

	if(eth_avb_ptp == ETH_TYPE_AVB_PTP)
	{
			ret = 3;
	}
	else if (eth_avb == ETH_TYPE_AVB)
	{
		if(eth_avb_class == ETH_TYPE_AVB_A)
		{
			ret = 1;
		}
		else if(eth_avb_class == ETH_TYPE_AVB_B)
		{
			ret = 2;
		}
		else
		{
			ret = 1;
		}
	}        

	return ret;
}

#if LINUX_VERSION_CODE > KERNEL_VERSION(2, 6, 29)
static const struct net_device_ops tc9560_netdev_ops = {
	.ndo_open		= tc9560_usbnet_open,
	.ndo_stop		= tc9560_usbnet_stop,
	.ndo_start_xmit		= tc9560_usbnet_start_xmit,
	.ndo_set_rx_mode 	= tc9560_set_rx_mode,
	.ndo_select_queue = tc9560_select_queue, 			/* For MQS */
	.ndo_tx_timeout		= tc9560_usbnet_tx_timeout,
	.ndo_change_mtu		= tc9560_change_mtu,
	.ndo_do_ioctl		= tc9560_ioctl,
	.ndo_set_mac_address	= tc9560_set_mac_addr,
	.ndo_validate_addr	= eth_validate_addr,
};
#endif


static int tc9560_bind(struct usbnet *dev, struct usb_interface *intf)
{
#if (IOCTL_DEFINE == 1)
	int i = 0;
#endif
	int ret;
	u64 ptp_temp = 0;


#ifdef TEST_MAC_ADD
	int count; 
#endif

	struct tc9560_data *priv = NULL;
	struct timespec now;	
	u64 temp1 = 0,temp2 = 0,diff = 0;
	u8 mac_addr0[6] = DEFAULT_MAC_ADDRESS;

	tc9560_pdata.dev = dev;
	dev->data[0] = (unsigned long)kzalloc(sizeof(struct tc9560_data), GFP_ATOMIC);

	DBGPR("bind function call \n");
	priv = (struct tc9560_data *)dev->data[0];
	

	if(!priv)
	{
		printk("Unable to allocate struct tc9560_data\n");        
		return -ENOMEM;
	}

	memset(priv, 0, sizeof(*priv));
	priv->udev = dev;
	priv->udev->intf = intf;
	/* Assignment of All Out Endpoints */
	priv->legacy_out =  usb_sndbulkpipe (dev->udev, 0x01);
	priv->avb_a_out = usb_sndbulkpipe (dev->udev, 0x02);
	priv->avb_b_out = usb_sndbulkpipe (dev->udev, 0x03);
	priv->gptp_out = usb_sndbulkpipe (dev->udev, 0x04);

	/* Assignment of All In Endpoints */
	priv->legacy_in =  usb_rcvbulkpipe (dev->udev, 0x05);
	priv->avb_in = usb_rcvbulkpipe (dev->udev, 0x06);
	priv->avb_control = usb_rcvbulkpipe(dev->udev, 0x07);
	priv->gptp_in = usb_rcvbulkpipe(dev->udev, 0x08);
	priv->l2_filtering_mode = 0;
	
	MAC_MPFR_RgRd(priv->MAC_Packet_Filter);

	dev->in = priv->legacy_in;
	dev->out= priv->legacy_out;

	spin_lock_init(&priv->reg_lock);
	if (msg_enable != 0)
		dev->msg_enable = msg_enable;

	/*Lets read the version of ip in to device structure*/
	synopGMAC_read_version(dev);
	printk("Driver Version is %s\n", DRV_VERSION);
	
	/* Read mac address from mac.ini file */
	++mdio_bus_id;
	parse_config_file();
	mac_addr0[0] = dev_addr[0];
	mac_addr0[1] = dev_addr[1];
	mac_addr0[2] = dev_addr[2];
	mac_addr0[3] = dev_addr[3];
	mac_addr0[4] = dev_addr[4];
	mac_addr0[5] = dev_addr[5];

	/* Program/flash in the station/IP's Mac address */
	synopGMAC_set_mac_addr(dev, MAC_MA2HR_RgOffAddr, MAC_MA2LR_RgOffAddr, mac_addr0);
	synopGMAC_get_mac_addr(dev, MAC_MA2HR_RgOffAddr, MAC_MA2LR_RgOffAddr, dev->net->dev_addr);
	tc9560_reg_write(dev, 0x3004, 0x02FF0000);

	printk("TC9560 mac addr = %02x:%02x:%02x:%02x:%02x:%02x\n",
			dev->net->dev_addr[0],
			dev->net->dev_addr[1],
			dev->net->dev_addr[2],
			dev->net->dev_addr[3],
			dev->net->dev_addr[4],
			dev->net->dev_addr[5]);

	memcpy(dev->net->perm_addr, dev->net->dev_addr, ETH_ALEN);

	/*Check for Phy initialization*/
	synopGMAC_set_mdc_clk_div(dev, GmiiCsrClk1);


	/* Initialize MII structure */
	dev->mii.dev = dev->net;
	dev->mii.mdio_read = tc9560_mdio_read;
	dev->mii.mdio_write = tc9560_mdio_write;
	dev->mii.phy_id_mask = 0xff;                    
	dev->mii.reg_num_mask = 0xff;
	dev->mii.phy_id = 0x01;
	dev->mii.supports_gmii = 1;

	priv->phy_id = 0x01;

	ret = synopGMAC_check_phy_init(dev,0);

	nic_speed = 0;
	if(priv->Speed == 1)
		nic_speed = 10;
	if(priv->Speed == 2)
		nic_speed = 100;
	if(priv->Speed == 3)
		nic_speed = 1000;


#ifndef TC9560_DRV_TEST_LOOPBACK
	tc9560_reg_write(dev, MAC_MCR_RgOffAddr, 0x00332003);
#endif
	//synopGMAC_promisc_enable(dev);
	synopGMAC_set_speed(dev, priv->Speed); 

#ifdef IPC_OFFLOAD
	/* IPC Checksum offloading is enabled for this driver. Should only be used if Full Ip checksumm offload engine is configured in the hardware */
	synopGMAC_enable_rx_chksum_offload(dev);    /* Enable the offload engine in the receive path */

#endif


	/* The FEF bit in DMA control register is configured to 0 indicating DMA to drop the errored frames. */
	/* Inform the Linux Networking stack about the hardware capability of checksum offloading */
	dev->net->features = NETIF_F_HW_CSUM;

	TR("Setting up the cable unplug timer\n");
#ifndef TC9560_DRV_TEST_LOOPBACK
		synopGMAC_cable_unplug_timer.data = (unsigned long)dev;
		INIT_DELAYED_WORK(&task, (void *)synopGMAC_linux_cable_unplug_function);
		schedule_delayed_work(&task, msecs_to_jiffies(TC9560_PHY_DETECT_INTERVAL));
#endif
	dev->rx_urb_size = RX_URB_SIZE;
  dev->net->flags |= IFF_ALLMULTI;

#if 1
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 30)
	dev->net->do_ioctl = tc9560_ioctl;
	dev->net->set_mac_address = tc9560_set_mac_addr;
	dev->net->change_mtu = tc9560_change_mtu;
#else
	dev->net->netdev_ops = &tc9560_netdev_ops;
#endif

	dev->net->ethtool_ops = &tc9560_ethtool_ops;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 30)
	dev->net->needed_headroom = 8;
#endif

	dev->net->features |= NETIF_F_IP_CSUM;
#if LINUX_VERSION_CODE > KERNEL_VERSION(2, 6, 22)
	dev->net->features |= NETIF_F_IPV6_CSUM;
#endif
	dev->net->features |= NETIF_F_TSO;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 39)
	dev->net->hw_features |= NETIF_F_IP_CSUM;
#if LINUX_VERSION_CODE > KERNEL_VERSION(2, 6, 22)
	dev->net->hw_features |= NETIF_F_IPV6_CSUM;
#endif
	dev->net->hw_features |= NETIF_F_TSO;
#endif
#endif
	pdata_phc = priv;
	device = interface_to_usbdev(intf);
	getnstimeofday(&now);
        temp1 = (u64)(now.tv_sec * 1000000000);
        temp1 = (u64)(temp1 +  now.tv_nsec);
	
#ifdef TC9560_DRV_TEST_LOOPBACK
	tc9560_reg_write(dev, EVBTsCtl, 0x00800025); 
#else
	tc9560_reg_write(dev, EVBTsCtl, 0x07800725); 
#endif	

        getnstimeofday(&now);
        temp2 = (u64)(now.tv_sec * 1000000000);
        temp2 = (u64)(temp2 +  now.tv_nsec);
	diff = temp2 - temp1;
	DBGPR("Register Write Time %lld\n",diff);

	/* To enable ping commented below lins*/
	tc9560_reg_write(dev, VendSpecfPktHdrH, 0);
	tc9560_reg_write(dev, VendSpecfPktHdrL, 0);

	tc9560_reg_write(dev, VendSpecfSrcAddrH, (dev->net->dev_addr[0] << 8) | dev->net->dev_addr[1]);
	tc9560_reg_write(dev, VendSpecfSrcAddrL, (dev->net->dev_addr[2] << 24) | (dev->net->dev_addr[3] << 16) |(dev->net->dev_addr[4] << 8) | dev->net->dev_addr[5]);

	tc9560_reg_write(dev, VendSpecfDestAddrH, (dev->net->dev_addr[0] << 8) | dev->net->dev_addr[1]);
	tc9560_reg_write(dev, VendSpecfDestAddrL, (dev->net->dev_addr[2] << 24) | (dev->net->dev_addr[3] << 16) |(dev->net->dev_addr[4] << 8) | dev->net->dev_addr[5]);

	tc9560_reg_write(dev, EVBTimeOff8, 0x1e8480);   
	tc9560_reg_write(dev, EVBTimeOff0, 0x1e8480);   
	priv->one_nsec_accuracy = 1;	

	tc9560_ptp_init(priv); 

	tc9560_reg_write(dev,0xAB00,0x10057E27);

	/* program Sub Second Increment Reg */
	config_sub_second_increment(DWC_ETH_QOS_SYSCLOCK); 

	ptp_temp = (u64)(50000000ULL << 32);
	priv->default_addend = div_u64(ptp_temp, DWC_ETH_QOS_SYSCLOCK);
	config_addend(priv->default_addend);

	getnstimeofday(&now);
	init_systime(now.tv_sec, now.tv_nsec);

	priv->cfg = TC9560_USB_CONFIG_1;

#if (IOCTL_DEFINE == 1)
    tc9560_urb_context.tc9560_i = 0;
    for(i = 0; i < 20; i++)
    {
        tc9560_urb_alloc[i] = kmalloc(sizeof(struct urb), GFP_ATOMIC);
        if (!tc9560_urb_alloc[i]) 
        {
            printk(KERN_ERR "\n\ralloc_urb: kmalloc failed:%s:%d\n\r",__func__,__LINE__);
            goto error;
        }

        dma_buf[i] = usb_alloc_coherent(dev->udev, ALLOC_SIZE, GFP_ATOMIC, &tc9560_urb_alloc[i]->transfer_dma);
        if (!dma_buf[i]) 
        {
            printk("\n\rusb_alloc_coherent NO MEMORY:%s:%d\n\r",__func__,__LINE__);
            //retval = -ENOMEM;
            goto error;
        }
    }

    sema_init(&tc9560_urb_context.tc9560_sem, 15);
#endif
	return 0;

#if (IOCTL_DEFINE == 1)
error:    
    i-=1;
    for(; i>=0 && i<20; i--)
    {
        usb_free_coherent(dev->udev, ALLOC_SIZE, dma_buf[i], tc9560_urb_alloc[i]->transfer_dma);
        //usb_free_coherent(tc9560_urb_alloc[i]->dev, ALLOC_SIZE, tc9560_urb_alloc[i]->transfer_buffer, tc9560_urb_alloc[i]->transfer_dma);
        usb_free_urb(tc9560_urb_alloc[i]);
    }
	return -ENOMEM;
#endif
}


static void tc9560_unbind(struct usbnet *dev, struct usb_interface *intf)
{
#if (IOCTL_DEFINE == 1)
	int i=0;
#endif
	struct tc9560_data *priv = NULL;

	synopGMAC_rx_disable(dev);
	synopGMAC_tx_disable(dev);

#if (IOCTL_DEFINE == 1)
    for(i = 0; i < 20; i++)
    {
        usb_free_coherent(dev->udev, ALLOC_SIZE, dma_buf[i], tc9560_urb_alloc[i]->transfer_dma);
        usb_free_urb(tc9560_urb_alloc[i]);
    }
#endif

	priv = (struct tc9560_data *)dev->data[0];
	if(!priv)
	{
		printk("Unable to remove ptp_clock\n");
		return ;
	}

	tc9560_ptp_remove(priv);
	usb_deregister_dev(intf, &class);

}






static int tc9560_rx_fixup(struct usbnet *dev, struct sk_buff *skb)
{
#ifndef TC9560_DRV_TEST_LOOPBACK
	int ret = 1;
#endif
	u16 eth_type;
	u8 sub_type = 0;
	u32 *data_ptr = NULL;

  data_ptr = (u32 *)skb->data;
  eth_type = *((u16 *)data_ptr + 6);
  eth_type = cpu_to_be16(eth_type);

#ifndef TC9560_OUI_DEBUG
  if (eth_type == ETH_TYPE_VEND)
  {
    sub_type = *((u8 *)data_ptr + 19);
  }

  if((eth_type == ETH_TYPE_AVB_PTP) || ((eth_type == ETH_TYPE_VEND)
      && ((sub_type == ETH_VEND_SUB_TYPE_gPTP_TX_TS) 
      || (sub_type == ETH_VEND_SUB_TYPE_gPTP_RX_TS)))) {
#ifndef TC9560_DRV_TEST_LOOPBACK
    ret = tc9560_ptp_rx_hwtstamp(dev,skb);
#endif
  }

#ifdef TC9560_DRV_TEST_LOOPBACK
	DBGPR("RX Channel, skb->len = %d , protocol = %#x \n", skb->len, eth_type);
	if(eth_type == TC9560_VLAN_TAG) {
		//4Bytes VID. Data starts from [19]. Modify for Loopback
		skb->data[19] = 0xFF;
	} else {	
		/* It's protcol (ether type) field */
		if (eth_type == ETH_TYPE_AVB_PTP) {
			//No VID. GPTP Data starts from [16]. Modify for Loopback
			skb->data[16] = 0xFF;
		} else if (eth_type == ETH_TYPE_AVB) {
			//No VID. AVB Control. Data starts from [15]. Modify for Loopback
			skb->data[15] = 0xFF;
		} else if (eth_type == ETH_TYPE_LEGACY) {
			//No VID. Data starts from [14]. Modify for Loopback
			skb->data[14] = 0xFF;
		}
	}
#endif
  return 1;

#else
	static struct sk_buff *dbg_skb = NULL;
	static struct sk_buff *dbg_data_ptr = NULL;
	/* skb verification */
	if(skb == NULL)
	{
		printk("NULL skb in tc9560_rx_fixup\n");
		return 1;
	}

	if(dbg_skb == skb){
		printk("received same skb\n");
		return 1;
	}
	dbg_skb = skb;

	/* skb data buffer verification */
	data_ptr = (u32 *)skb->data;
	if(data_ptr == NULL)
	{
		printk("NULL data buffer in tc9560_rx_fixup\n");
		return 1;
	}
	
	if(dbg_data_ptr == (struct sk_buff *)data_ptr){
		printk("received same skb data buffer\n");
		return 1;
	}
	dbg_data_ptr = (struct sk_buff *)data_ptr;

	/* skb length verification */
#if 0
	if(skb->len == 0){
		printk("Received zero length skb\n");
		return 1;
	}
	if(skb->len > 512){
		printk("Received oversize skb = %d\n", skb->len);
		return 1;
	}	
	if(skb->len < 36){
		printk("Received undersided packet = %d\n", skb->len);
		return 1;
	}
#endif
	eth_type = *((u16 *)data_ptr + 6);
	eth_type = cpu_to_be16(eth_type);

	if (eth_type == ETH_TYPE_VEND) 
	{
		sub_type = *((u8 *)data_ptr + 19);
		if( (sub_type != ETH_VEND_SUB_TYPE_gPTP_TX_TS) && (sub_type != ETH_VEND_SUB_TYPE_gPTP_RX_TS) ){
			printk("Unexpected vender specific sub type = %d\n", sub_type);
			return 1;
		}
	}

	if((eth_type == ETH_TYPE_AVB_PTP) || ((eth_type == ETH_TYPE_VEND)))
	{	
#ifndef TC9560_DRV_TEST_LOOPBACK
		ret = tc9560_ptp_rx_hwtstamp(dev,skb);
#endif
	}
	if(eth_type == ETH_TYPE_AVB)
		DBGPR("AVB data Received\n");

	return 1;
#endif
}


static struct sk_buff *
tc9560_tx_fixup(struct usbnet *dev, struct sk_buff *skb, gfp_t flags)
{
	struct tc9560_data *priv = NULL;
	u16 *skb_data_ptr = NULL;
	u16 eth_avb_ptp,eth_avb;
	u16  eth_avb_class;
	priv = (struct tc9560_data *)dev->data[0];

	skb_data_ptr = (u16 *)skb->data;
	eth_avb_ptp = *(skb_data_ptr + 6);
	eth_avb_ptp = cpu_to_be16(eth_avb_ptp);

	eth_avb = *(skb_data_ptr + 8);
	eth_avb = cpu_to_be16(eth_avb);
	eth_avb_class = *(skb_data_ptr + 7);
	eth_avb_class = cpu_to_be16(eth_avb_class);


	if(eth_avb_ptp == ETH_TYPE_AVB_PTP)
	{

		if(!skb->sk)
			printk("skb->sk is null\n");
		else
		{               
#ifndef TC9560_DRV_TEST_LOOPBACK
			int ret;
			ret = tc9560_ptp_tx_hwtstamp(dev,skb);
#endif
			skb_set_queue_mapping(skb, TC9560_TX_PTP);	
			//dev->out = priv->gptp_out; 
		}            
	}
	else if (eth_avb == ETH_TYPE_AVB)
	{

		if(eth_avb_class == ETH_TYPE_AVB_A)
		{
			skb_set_queue_mapping(skb, TC9560_TX_AVBA);	
			//dev->out = priv->avb_a_out;   
		}
		else if(eth_avb_class == ETH_TYPE_AVB_B)
		{
			skb_set_queue_mapping(skb, TC9560_TX_AVBB);	
			//dev->out = priv->avb_b_out; 
		}
		else
		{
			skb_set_queue_mapping(skb, TC9560_TX_AVBA);	
			//dev->out = priv->avb_a_out;
		}
	}        
	else
	{
		skb_set_queue_mapping(skb, TC9560_TX_LEG);	
		//dev->out = priv->legacy_out;     
	}     

	return skb;
}

static int tc9560_link_reset(struct usbnet *dev)
{
	DBGPR("tc9560_dbg: In %s\n", __func__);
	return 0;
}

static int tc9560_reset(struct usbnet *dev)
{
	u32 ret;
	struct tc9560_data *priv = (struct tc9560_data *)dev->data[0];

	DBGPR("tc9560_dbg: In %s\n", __func__);
	/* Power up ethernet PHY */
	ret = synopGMAC_check_phy_init(dev,0);   
	if (ret < 0)
	{
		printk("ethernet phy init failed\n");
		return ret;   
	}          


	/*Initialize the mac interface*/
	//synopGMAC_promisc_enable(dev);
	synopGMAC_set_speed(dev, priv->Speed); 

	/* The FEF bit in DMA control register is configured to 0 indicating DMA to drop the errored frames */
	/* Inform the Linux Networking stack about the hardware capability of checksum offloading */
	dev->net->features = NETIF_F_HW_CSUM;

	dev->rx_urb_size = RX_URB_SIZE;
	return 0;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 32)
static int tc9560_stop(struct usbnet *dev)
{
	DBGPR("tc9560_dbg: In %s\n", __func__);
	return 0;
}
#endif


static const struct driver_info tc9560_info = {
	.description = "TC9560 USB HSIC to Ethernet AVB & legacy Ethernet Bridge Chip",
	.bind = tc9560_bind,
	.unbind = tc9560_unbind,
	.status = tc9560_status,
	.link_reset = tc9560_link_reset,
	.reset = tc9560_reset,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 32)
	.stop = tc9560_stop,
#endif
	.flags = FLAG_ETHER | FLAG_FRAMING_AX,
	.rx_fixup = tc9560_rx_fixup,
	.tx_fixup = tc9560_tx_fixup,
};

static const struct usb_device_id	products[] = {
	{
		/* tc9560_tbd: tc9560 Vendor ID, Product ID */
		USB_DEVICE_INTERFACE_NUMBER(0x0930, 0x1705, 0),
		.driver_info = (unsigned long) &tc9560_info,
	}, 
	{ },
};
MODULE_DEVICE_TABLE(usb, products);

static struct usb_driver tc9560_driver = {
	.name =		"tc9560",
	.id_table =	products,
	.probe =	tc9560_usbnet_probe,
	//.probe =	usbnet_probe,
	.suspend =	tc9560_suspend,
	.resume =	tc9560_resume,
	.disconnect =	usbnet_disconnect,
};


static int __init tc9560_init(void)
{
	return usb_register(&tc9560_driver);
}
module_init(tc9560_init);

static void __exit tc9560_exit(void)
{
	cancel_delayed_work_sync(&task);
	usb_deregister(&tc9560_driver);
}
module_exit(tc9560_exit);

MODULE_AUTHOR("TAEC/TDSC");
MODULE_DESCRIPTION("TC9560 HSIC to Ethernet AVB & legacy Ethernet Bridge Chip");
MODULE_LICENSE("GPL");
MODULE_VERSION(DRV_VERSION);
