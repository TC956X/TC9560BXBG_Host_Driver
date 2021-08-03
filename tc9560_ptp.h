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
 

#ifndef	__TC9560_PTP_H
#define	__TC9560_PTP_H

#include "tc9560_common.h"
#include "tc9560_gmac.h"
#include "tc9560_reg_rw.h"

#define  SYNC_MSG_TYPE 0x00
#define  SYNC_FOLLOWUP_MSG_TYPE 0x08
#define  PDELAY_REQ_MSG_TYPE 0x02
#define  PDELAY_RESP_MSG_TYPE 0x03
#define  PDELAY_RESP_FOLLOWUP_MSG_TYPE 0x0a

#define  ONE_NS  0x3B9ACA00
#define Y_SUCCESS 1
#define Y_FAILURE 0

struct tc9560_ptp_data
{
    /*PTP Clock Parameters*/
    u32 flags;
    struct ptp_clock *ptp_clock;
    struct ptp_clock_info ptp_caps;
    struct delayed_work ptp_overflow_work;
    struct work_struct ptp_tx_work;
    struct sk_buff *ptp_tx_skb;
    spinlock_t tmreg_lock;
    struct timecounter tc;
    struct usbnet *dev;
}__attribute__ ((packed));


void tc9560_ptp_tx_work(struct work_struct *work);
int tc9560_ptp_tx_hwtstamp(struct usbnet *dev,struct sk_buff *skb);
int tc9560_ptp_rx_hwtstamp(struct usbnet *dev,struct sk_buff *skb);
int tc9560_ptp_hwtstamp_ioctl(struct net_device *net,struct ifreq *ifr, int cmd);
int tc9560_ptp_init(struct tc9560_data *pdata);
int tc9560_phc_index(struct tc9560_data *pdata);
void tc9560_ptp_remove(struct tc9560_data *pdata);
int config_sub_second_increment(unsigned long ptp_clock);
int config_addend(unsigned int data);
int init_systime(unsigned int sec,unsigned int nsec);

#endif //__TC9560_PTP_H
