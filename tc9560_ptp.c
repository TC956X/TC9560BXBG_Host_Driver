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
 

#include "tc9560_ptp.h"

/* \brief This sequence is used configure MAC SSIR
* \param[in] ptp_clock
* \return Success or Failure
* \retval  0 Success
* \retval -1 Failure
*/

int config_sub_second_increment(unsigned long ptp_clock)
{
  unsigned long val;
  unsigned long varMAC_TCR;

  MAC_TCR_RgRd(varMAC_TCR);

  /* convert the PTP_CLOCK to nano second */
  /*  formula is : ((1/ptp_clock) * 1000000000) */
	/*  where, ptp_clock = 50MHz if FINE correction */
	/*  and ptp_clock = DWC_ETH_QOS_SYSCLOCK if COARSE correction */
  if (GET_VALUE(varMAC_TCR, MAC_TCR_TSCFUPDT_LPOS, MAC_TCR_TSCFUPDT_HPOS) == 1) {
    val = ((1 * 1000000000ull) / 50000000);
  }
  else {
    val = ((1 * 1000000000ull) / ptp_clock);
  }

  /* 0.465ns accurecy */
  if (GET_VALUE(varMAC_TCR, MAC_TCR_TSCTRLSSR_LPOS, MAC_TCR_TSCTRLSSR_HPOS) == 0) {
    val = (val * 1000) / 465;
  }

  MAC_SSIR_SSINC_UdfWr(val);
  DBGPR("increment value %lx\n",val);

  return Y_SUCCESS;
}



/*!
* \brief This sequence is used get 64-bit system time in nano sec
* \return (unsigned long long) on success
* \retval ns
*/

static unsigned long long get_systime(void)
{
  unsigned long long ns = 0;
  unsigned long varmac_stnsr = 0;
  unsigned long varmac_stsr = 0;
  unsigned long double_read_stsr = 0;
  unsigned long double_read_stnsr = 0;

	DBGPR("%s\n", __func__);
  MAC_STSR_RgRd(varmac_stsr);

  MAC_STNSR_RgRd(varmac_stnsr);
  
	/* Double read seconds and nanoseconds */ 
  MAC_STSR_RgRd(double_read_stsr);

  /* Check for nano second roll over */
  if(varmac_stsr != double_read_stsr)
  {
    MAC_STNSR_RgRd(double_read_stnsr);
    varmac_stsr = double_read_stsr;
    varmac_stnsr = double_read_stnsr;
    DBGPR("Nano second roll over corrected");
  }

  /* Extract 30-bit value for Nano second register */

  ns = GET_VALUE(varmac_stnsr, MAC_STNSR_TSSS_LPOS, MAC_STNSR_TSSS_HPOS);
 
 	/* convert sec/high time value to nanosecond */ 
  /* Convert Total value in Nano second */
  ns = ns + (varmac_stsr * 1000000000ull);

  return ns;
}


/*!
* \brief This sequence is used to adjust/update the system time
* \param[in] sec
* \param[in] nsec
* \return Success or Failure
* \retval  0 Success
* \retval -1 Failure
*/

static int adjust_systime(unsigned int sec,
						unsigned int nsec,
			  			int add_sub,
						bool one_nsec_accuracy)
{
  unsigned long retryCount = 100000;
  unsigned long vy_count;
  volatile unsigned long varMAC_TCR;
  u64 data = 0;

	DBGPR("%s\n", __func__);
  /* wait for previous(if any) time adjust/update to complete. */

  /*Poll*/
  vy_count = 0;
  while(1){
    if(vy_count > retryCount) {
      return -Y_FAILURE;
    }


    MAC_TCR_RgRd(varMAC_TCR);
  
  if (GET_VALUE(varMAC_TCR, MAC_TCR_TSUPDT_LPOS, MAC_TCR_TSUPDT_HPOS) == 0) {
       break;
    }
    vy_count++;
		//mdelay(1);
  }

  if (add_sub) {
    /* If the new sec value needs to be subtracted with
     * the system time, then MAC_STSUR reg should be
     * programmed with (2^32 – <new_sec_value>)
     * */
    sec = (0x100000000ull - sec);

    /* If the new nsec value need to be subtracted with
     * the system time, then MAC_STNSUR.TSSS field should be
     * programmed with,
     * (10^9 - <new_nsec_value>) if MAC_TCR.TSCTRLSSR is set or
     * (2^31 - <new_nsec_value> if MAC_TCR.TSCTRLSSR is reset)
     * */
  	if (one_nsec_accuracy)
      nsec = (0x3B9ACA00 - nsec);
   	else
      nsec = (0x80000000 - nsec);
  }

  MAC_STSUR_RgWr(sec);
  
  MAC_STNSUR_TSSS_UdfWr(nsec);
  MAC_STNSUR_ADDSUB_UdfWr(add_sub);
#ifdef TC9560_WRAPPER_DMA
  data = (u64)(sec * 1000000000);
  data = (u64)(data + nsec);
  TC9560_PTPLCLUPDT_RgWr((u32)data);
#endif

  /* issue command to initialize system time with the value */
  /* specified in MAC_STSUR and MAC_STNSUR. */
  MAC_TCR_TSUPDT_UdfWr(0x1);
  /* wait for present time initialize to complete. */

  /*Poll*/
  vy_count = 0;
  while(1){
    if(vy_count > retryCount) {
      return -Y_FAILURE;
    }

    MAC_TCR_RgRd(varMAC_TCR);
    if (GET_VALUE(varMAC_TCR, MAC_TCR_TSUPDT_LPOS, MAC_TCR_TSUPDT_HPOS) == 0) {
      break;
    }
    vy_count++;
		//mdelay(1);
  }

  return Y_SUCCESS;
}



/*!
* \brief This sequence is used to adjust the ptp operating frequency.
* \return Success or Failure
* \retval  0 Success
* \retval -1 Failure
*/

int config_addend(unsigned int data)
{
  unsigned long retryCount = 100000;
  unsigned long vy_count;
  volatile unsigned long varMAC_TCR;

	DBGPR("%s\n", __func__);
  /* wait for previous(if any) added update to complete. */

  /*Poll*/
  vy_count = 0;
  while(1){
    if(vy_count > retryCount) {
      return -Y_FAILURE;
    }

    MAC_TCR_RgRd(varMAC_TCR);

    if (GET_VALUE(varMAC_TCR, MAC_TCR_TSADDREG_LPOS, MAC_TCR_TSADDREG_HPOS) == 0) {
      break;
    }
    vy_count++;
    //mdelay(1);
  }

  MAC_TAR_RgWr(data);
  /* issue command to update the added value */
  MAC_TCR_TSADDREG_UdfWr(0x1);
  /* wait for present added update to complete. */

  /*Poll*/
  vy_count = 0;
  while(1){
    if(vy_count > retryCount) {
      return -Y_FAILURE;
    }
    MAC_TCR_RgRd(varMAC_TCR);
    if (GET_VALUE(varMAC_TCR, MAC_TCR_TSADDREG_LPOS, MAC_TCR_TSADDREG_HPOS) == 0) {
      break;
    }
    vy_count++;
    //mdelay(1);
  }

  return Y_SUCCESS;
}


/*!
* \brief This sequence is used to initialize the system time
* \param[in] sec
* \param[in] nsec
* \return Success or Failure
* \retval  0 Success
* \retval -1 Failure
*/

//static int init_systime(unsigned int sec,
int init_systime(unsigned int sec,
                        unsigned int nsec)
{
  unsigned long retryCount = 100;
  unsigned long vy_count;
  volatile unsigned long varMAC_TCR = 0;
  u64 data = 0;

	DBGPR("%s\n", __func__);
  /* wait for previous(if any) time initialize to complete. */

  /*Poll*/
  vy_count = 0;
  while(1){
    if(vy_count > retryCount) {
      return -Y_FAILURE;
    }

    MAC_TCR_RgRd(varMAC_TCR);
    if (GET_VALUE(varMAC_TCR, MAC_TCR_TSINIT_LPOS, MAC_TCR_TSINIT_HPOS) == 0) {
      break;
    }
    vy_count++;
    //mdelay(1);
  }
  MAC_STSUR_RgWr(sec);
  MAC_STNSUR_RgWr(nsec);

#ifdef TC9560_WRAPPER_DMA
  data = (u64)(sec * 1000000000);
  data = (u64)(data + nsec);
  TC9560_PTPLCLINIT_RgWr((u32)data);
#endif

  /* issue command to initialize system time with the value */
  /* specified in MAC_STSUR and MAC_STNSUR. */
  MAC_TCR_TSINIT_UdfWr(0x1);
  /* wait for present time initialize to complete. */

  /*Poll*/
  vy_count = 0;
  while(1){

    if(vy_count > retryCount) {
      return -Y_FAILURE;
    }

    MAC_TCR_RgRd(varMAC_TCR);
    DBGPR("value of TCR in init systime %lx\n",varMAC_TCR);
    if (GET_VALUE(varMAC_TCR, MAC_TCR_TSINIT_LPOS, MAC_TCR_TSINIT_HPOS) == 0) {
      break;
    }
    vy_count++;
    //mdelay(1);
  }

  return Y_SUCCESS;
}

/**
 * tc9560_ptp_systim_to_hwtstamp - convert system time value to hw timestamp
 * @tc9560_pdata: board private structure
 * @hwtstamps: timestamp structure to update
 * @systim: unsigned 64bit system time value.
 *
 * We need to convert the system time value stored in the registers
 * into a hwtstamp which can be used by the upper level timestamping functions.
 **/
static void tc9560_ptp_systim_to_hwtstamp(struct usbnet *dev,
		struct skb_shared_hwtstamps *hwtstamps,
		u64 systim)
{
	memset(hwtstamps, 0, sizeof(*hwtstamps));
	/* Upper 32 bits contain sec, lower 32 bits contain nsec. */
	hwtstamps->hwtstamp = ktime_set(systim >> 32,
			systim & 0xFFFFFFFF);
}



/*!
 * \brief API to adjust the frequency of hardware clock.
 *
 * \details This function is used to adjust the frequency of the
 * hardware clock.
 *
 * \param[in] ptp – pointer to ptp_clock_info structure.
 * \param[in] delta – desired period change in parts per billion.
 *
 * \return int
 *
 * \retval 0 on success and -ve number on failure.
 */
int tc9560_ptp_adjfreq(struct ptp_clock_info *ptp, s32 ppb)
{
    struct tc9560_data *pdata =
            container_of(ptp, struct tc9560_data, ptp_clock_ops);
    //unsigned long flags;
    u64 adj;
    u32 diff, addend;
    int neg_adj = 0;

    DBGPR("tc9560_ptp_adjfreq\n");
    if (ppb < 0) {
        neg_adj = 1;
        ppb = -ppb;
    }

    addend = pdata->default_addend;
    adj = addend;
    adj *= ppb;
    /*
    * div_u64 will divided the "adj" by "1000000000ULL"
    * and return the quotient.
    */
    diff = div_u64(adj, 1000000000ULL);
    addend = neg_adj ? (addend - diff) : (addend + diff);

    mutex_lock(&pdata->ptp_mutex);
    config_addend(addend);
    mutex_unlock(&pdata->ptp_mutex);

    return 0;
}


/*!
 * \brief API to adjust the hardware time.
 *
 * \details This function is used to shift/adjust the time of the
 * hardware clock.
 *
 * \param[in] ptp – pointer to ptp_clock_info structure.
 * \param[in] delta – desired change in nanoseconds.
 *
 * \return int
 *
 * \retval 0 on success and -ve number on failure.
 */
int tc9560_ptp_adjtime(struct ptp_clock_info *ptp, s64 delta)
{
    struct tc9560_data *pdata =
            container_of(ptp, struct tc9560_data, ptp_clock_ops);
    u32 sec, nsec;
    u32 quotient, reminder;
    int neg_adj = 0;

    DBGPR("%s\n", __func__);
    if (delta < 0) {
        neg_adj = 1;
        delta =-delta;
    }

    quotient = div_u64_rem(delta, 1000000000ULL, &reminder);
    sec = quotient;
    nsec = reminder;

    mutex_lock(&pdata->ptp_mutex);
    adjust_systime(sec, nsec, neg_adj, pdata->one_nsec_accuracy);
    mutex_unlock(&pdata->ptp_mutex);

    return 0;
}


/*!
 * \brief API to get the current time.
 *
 * \details This function is used to read the current time from the
 * hardware clock.
 *
 * \param[in] ptp – pointer to ptp_clock_info structure.
 * \param[in] ts – pointer to hold the time/result.
 *
 * \return int
 *
 * \retval 0 on success and -ve number on failure.
 */
int tc9560_ptp_get_time(struct ptp_clock_info *ptp, struct timespec *ts)
{
    struct tc9560_data *pdata =
            container_of(ptp, struct tc9560_data, ptp_clock_ops);
    u64 ns;
    u32 reminder;
    u64 temp = 0;

    DBGPR("%s\n", __func__);
    mutex_lock(&pdata->ptp_mutex);
    ns = get_systime();
    mutex_unlock(&pdata->ptp_mutex);

    temp = div_u64_rem(ns, 1000000000ULL, &reminder);
    ts->tv_sec = temp;
    ts->tv_nsec = reminder;

    return 0;
}



/*!
 * \brief API to set the current time.
 *
 * \details This function is used to set the current time on the
 * hardware clock.
 *
 * \param[in] ptp – pointer to ptp_clock_info structure.
 * \param[in] ts – time value to set.
 *
 * \return int
 *
 * \retval 0 on success and -ve number on failure.
 */
static int tc9560_ptp_set_time(struct ptp_clock_info *ptp,
        const struct timespec *ts)
{
    struct tc9560_data *pdata =
            container_of(ptp, struct tc9560_data, ptp_clock_ops);

    DBGPR("-->DWC_ETH_QOS_set_time: ts->tv_sec = %ld,"
            "ts->tv_nsec = %ld\n", ts->tv_sec, ts->tv_nsec);


    mutex_lock(&pdata->ptp_mutex);
    init_systime(ts->tv_sec, ts->tv_nsec);
    mutex_unlock(&pdata->ptp_mutex);

    DBGPR("<--DWC_ETH_QOS_set_time\n");

return 0;
}


static struct sk_buff *prev_skb_rx=NULL;
static u32 len;

#define ARRAY_MAX_BUFFER 8
static struct sk_buff *prev_skb_array[ARRAY_MAX_BUFFER]= {NULL};
static unsigned int push_index = 0, pop_index = 0, count_null = 0;


/*!
 * \brief API to Extract GPTP Tx Packet OUI time stamp and send to application layer
 *
 * \details This function is used to set the current time on the
 * hardware clock.
 *
 * \param[in] dev – pointer to USBNET device.
 * \param[in] skb – pointer to received OUI skb from HW.
 *
 * \return int
 *
 * \retval 0 on success and -ve number on failure.
 */
int tc9560_ptp_tx_hwtstamp(struct usbnet *dev,
		struct sk_buff *skb)
{
	u8 msg_type;
	u16 eth_type;
	u32 *stamp = (u32 *)skb->data;

	eth_type = *((u16 *)stamp + 6);
	eth_type = cpu_to_be16(eth_type);
	msg_type = (*((u8 *)stamp + 14));
	msg_type &= 0xF; 

	if (eth_type == ETH_TYPE_AVB_PTP && (msg_type == SYNC_MSG_TYPE || msg_type == PDELAY_RESP_MSG_TYPE || msg_type == PDELAY_REQ_MSG_TYPE))
	{
		prev_skb_array[push_index] = skb_get(skb);
		push_index += 1;
		if(push_index >= ARRAY_MAX_BUFFER)
			push_index = 0;
	}
	return 1;
}


/*!
 * \brief API to Extract GPTP Rx Packet OUI time stamp and send to application layer
 *
 * \details This function is used to set the current time on the
 * hardware clock.
 *
 * \param[in] dev – pointer to USBNET device.
 * \param[in] skb – pointer to received OUI skb from HW.
 *
 * \return int
 *
 * \retval 0 on success and -ve number on failure.
 */
int tc9560_ptp_rx_hwtstamp(struct usbnet *dev,
		struct sk_buff *skb)
{
	struct skb_shared_hwtstamps shhwtstamps;
	u64 hw_time;    
	u16 eth_type;
	u8 msg_type, vend_sub_type;
	u32 *stamp = (u32 *)skb->data;
	
	eth_type = *((u16 *)stamp + 6);
	eth_type = cpu_to_be16(eth_type);
	msg_type = (*((u8 *)stamp + 14));
	msg_type &= 0xF;

	if (eth_type == ETH_TYPE_AVB_PTP && (msg_type == SYNC_MSG_TYPE || msg_type == PDELAY_RESP_MSG_TYPE || msg_type == PDELAY_REQ_MSG_TYPE))
	{
		prev_skb_rx = skb_get(skb);
		len = skb->len;
		skb->len = 0;
		return 1;
	}

	if (eth_type == ETH_TYPE_VEND) {
		vend_sub_type = (*((u8 *)stamp + 19));
		hw_time = (*(stamp + 6));
		hw_time |= (u64)(*(stamp + 7)) << 32;
		hw_time = cpu_to_be64(hw_time);

		if (vend_sub_type ==  0x01)  /*ETH_VEND_SUB_TYPE_gPTP_TX_TS*/
		{
			if(prev_skb_array[pop_index] != NULL)
			{				
				tc9560_ptp_systim_to_hwtstamp(dev, &shhwtstamps, hw_time);
				skb_tstamp_tx(prev_skb_array[pop_index], &shhwtstamps);	
				dev_kfree_skb_any(prev_skb_array[pop_index]);
				prev_skb_array[pop_index] = NULL;
				pop_index += 1;
				if(pop_index == ARRAY_MAX_BUFFER)
					pop_index = 0;
				skb->len = 0;
			}
			else {  // Should never happen
				printk("NULL %d\n",++count_null);
				skb->len = 0;
			}
		}

		else if(vend_sub_type ==  0x00)
		{
			if (prev_skb_rx == NULL) {
				printk("!!prev_skb_rx is NULL!\n");
    		skb->len = 0;
				return 1;
			}

			prev_skb_rx->len = len;
			len = prev_skb_rx->len - skb->len;

			tc9560_ptp_systim_to_hwtstamp(dev, skb_hwtstamps(skb), hw_time);
			skb_put(skb, len);
			memcpy(skb->data, prev_skb_rx->data, prev_skb_rx->len);
			dev_kfree_skb_any(prev_skb_rx);
		}
	}

	return 0;
}


/**
 * tc9560_ptp_hwtstamp_ioctl - control hardware time stamping
 * @netdev:
 * @ifre:
 * @cmd:
 *
 * Outgoing time stamping can be enabled and disabled. Play nice and
 * disable it when reuested, although it shouldn't case any overhead
 * when no packet needs it. At most one packet in the ueue may be
 * marked for time stamping, otherwise it would be impossible to tell
 * for sure to which packet the hardware time stamp belongs.
 *
 * Incoming time stamping has to be configured via the hardware
 * filters. Not all combinations are supported, in particular event
 * type has to be specified. Matching the kind of event packet is
 * not supported 
 *
 **/
int tc9560_ptp_hwtstamp_ioctl(struct net_device *net,
		struct ifreq *ifr, int cmd)
{
	struct hwtstamp_config config;
	u32 tsync_tx_ctl = 0;
	u32 tsync_rx_ctl = 0;	
	u32 ptp_v2 = 0;
	u32 tstamp_all = 0;
	u32 ptp_over_ipv4_udp = 0;
	u32 ptp_over_ipv6_udp = 0;
	u32 ptp_over_ethernet = 0;
	u32 snap_type_sel = 0;
	u32 ts_master_en = 0;
	u32 ts_event_en = 0;
	u32 av_8021asm_en = 0;
	u32 varMAC_TCR = 0;
	struct timespec now;
	u32 addend = 0;
	struct usbnet *dev = netdev_priv(net);
	struct tc9560_data *priv = NULL;

	if (copy_from_user(&config, ifr->ifr_data, sizeof(config)))
		return -EFAULT;

	if (config.flags)
		return -EINVAL;

	switch (config.tx_type) {
		case HWTSTAMP_TX_OFF:
			tsync_tx_ctl = 0;
			break;
		case HWTSTAMP_TX_ON:
			tsync_tx_ctl = 1;
			break;
		default:
			return -ERANGE;
	}
	priv = (struct tc9560_data *)dev->data[0];
		
	switch (config.rx_filter) {
	/* time stamp no incoming packet at all */
	case HWTSTAMP_FILTER_NONE:
		config.rx_filter = HWTSTAMP_FILTER_NONE;
		break;

	/* PTP v1, UDP, any kind of event packet */
	case HWTSTAMP_FILTER_PTP_V1_L4_EVENT:
		config.rx_filter = HWTSTAMP_FILTER_PTP_V1_L4_EVENT;
		/* take time stamp for all event messages */
		snap_type_sel = MAC_TCR_SNAPTYPSEL_1;

		ptp_over_ipv4_udp = MAC_TCR_TSIPV4ENA;
		ptp_over_ipv6_udp = MAC_TCR_TSIPV6ENA;
		break;

	/* PTP v1, UDP, Sync packet */
	case HWTSTAMP_FILTER_PTP_V1_L4_SYNC:
		config.rx_filter = HWTSTAMP_FILTER_PTP_V1_L4_SYNC;
		/* take time stamp for SYNC messages only */
		ts_event_en = MAC_TCR_TSEVENTENA;

		ptp_over_ipv4_udp = MAC_TCR_TSIPV4ENA;
		ptp_over_ipv6_udp = MAC_TCR_TSIPV6ENA;
		break;

	/* PTP v1, UDP, Delay_req packet */
	case HWTSTAMP_FILTER_PTP_V1_L4_DELAY_REQ:
		config.rx_filter = HWTSTAMP_FILTER_PTP_V1_L4_DELAY_REQ;
		/* take time stamp for Delay_Req messages only */
		ts_master_en = MAC_TCR_TSMASTERENA;
		ts_event_en = MAC_TCR_TSEVENTENA;

		ptp_over_ipv4_udp = MAC_TCR_TSIPV4ENA;
		ptp_over_ipv6_udp = MAC_TCR_TSIPV6ENA;
		break;

	/* PTP v2, UDP, any kind of event packet */
	case HWTSTAMP_FILTER_PTP_V2_L4_EVENT:
		config.rx_filter = HWTSTAMP_FILTER_PTP_V2_L4_EVENT;
		ptp_v2 = MAC_TCR_TSVER2ENA;
		/* take time stamp for all event messages */
		snap_type_sel = MAC_TCR_SNAPTYPSEL_1;

		ptp_over_ipv4_udp = MAC_TCR_TSIPV4ENA;
		ptp_over_ipv6_udp = MAC_TCR_TSIPV6ENA;
		break;

	/* PTP v2, UDP, Sync packet */
	case HWTSTAMP_FILTER_PTP_V2_L4_SYNC:
		config.rx_filter = HWTSTAMP_FILTER_PTP_V2_L4_SYNC;
		ptp_v2 = MAC_TCR_TSVER2ENA;
		/* take time stamp for SYNC messages only */
		ts_event_en = MAC_TCR_TSEVENTENA;

		ptp_over_ipv4_udp = MAC_TCR_TSIPV4ENA;
		ptp_over_ipv6_udp = MAC_TCR_TSIPV6ENA;
		break;

	/* PTP v2, UDP, Delay_req packet */
	case HWTSTAMP_FILTER_PTP_V2_L4_DELAY_REQ:
		config.rx_filter = HWTSTAMP_FILTER_PTP_V2_L4_DELAY_REQ;
		ptp_v2 = MAC_TCR_TSVER2ENA;
		/* take time stamp for Delay_Req messages only */
		ts_master_en = MAC_TCR_TSMASTERENA;
		ts_event_en = MAC_TCR_TSEVENTENA;

		ptp_over_ipv4_udp = MAC_TCR_TSIPV4ENA;
		ptp_over_ipv6_udp = MAC_TCR_TSIPV6ENA;
		break;

	/* PTP v2/802.AS1, any layer, any kind of event packet */
	case HWTSTAMP_FILTER_PTP_V2_EVENT:
		config.rx_filter = HWTSTAMP_FILTER_PTP_V2_EVENT;
		ptp_v2 = MAC_TCR_TSVER2ENA;
		/* take time stamp for all event messages */
		snap_type_sel = MAC_TCR_SNAPTYPSEL_1;

		ptp_over_ipv4_udp = MAC_TCR_TSIPV4ENA;
		ptp_over_ipv6_udp = MAC_TCR_TSIPV6ENA;
		ptp_over_ethernet = MAC_TCR_TSIPENA;
		av_8021asm_en = MAC_TCR_AV8021ASMEN;
		break;

	/* PTP v2/802.AS1, any layer, Sync packet */
	case HWTSTAMP_FILTER_PTP_V2_SYNC:
		config.rx_filter = HWTSTAMP_FILTER_PTP_V2_SYNC;
		ptp_v2 = MAC_TCR_TSVER2ENA;
		/* take time stamp for SYNC messages only */
		ts_event_en = MAC_TCR_TSEVENTENA;

		ptp_over_ipv4_udp = MAC_TCR_TSIPV4ENA;
		ptp_over_ipv6_udp = MAC_TCR_TSIPV6ENA;
		ptp_over_ethernet = MAC_TCR_TSIPENA;
		av_8021asm_en = MAC_TCR_AV8021ASMEN;
		break;

	/* PTP v2/802.AS1, any layer, Delay_req packet */
	case HWTSTAMP_FILTER_PTP_V2_DELAY_REQ:
		config.rx_filter = HWTSTAMP_FILTER_PTP_V2_DELAY_REQ;
		ptp_v2 = MAC_TCR_TSVER2ENA;
		/* take time stamp for Delay_Req messages only */
		ts_master_en = MAC_TCR_TSMASTERENA;
		ts_event_en = MAC_TCR_TSEVENTENA;

		ptp_over_ipv4_udp = MAC_TCR_TSIPV4ENA;
		ptp_over_ipv6_udp = MAC_TCR_TSIPV6ENA;
		ptp_over_ethernet = MAC_TCR_TSIPENA;
		av_8021asm_en = MAC_TCR_AV8021ASMEN;
		break;

	/* time stamp any incoming packet */
	case HWTSTAMP_FILTER_ALL:
		config.rx_filter = HWTSTAMP_FILTER_ALL;
		tstamp_all = MAC_TCR_TSENALL;
		break;

	default:
		return -ERANGE;
	}
	tsync_rx_ctl = ((config.rx_filter == HWTSTAMP_FILTER_NONE) ? 0 : 1);

	if(!tsync_tx_ctl && !tsync_rx_ctl) 
	{
		/* disable hw time stamping */
		tc9560_write(MAC_TCR_RgOffAddr, varMAC_TCR);
	} 
	else 
	{
		varMAC_TCR = (MAC_TCR_TSENA | MAC_TCR_TSCFUPDT | MAC_TCR_TSCTRLSSR |
				tstamp_all | ptp_v2 | ptp_over_ethernet | ptp_over_ipv6_udp |
				ptp_over_ipv4_udp | ts_event_en | ts_master_en |
				snap_type_sel | av_8021asm_en);

		if (!priv->one_nsec_accuracy)
			varMAC_TCR &= ~MAC_TCR_TSCTRLSSR;
		
		/* configuring the hardware timestamp in MAC_Timestamp_Control register (0x4000_AB00)*/
		tc9560_write(MAC_TCR_RgOffAddr, varMAC_TCR);
		
		
		config_sub_second_increment(DWC_ETH_QOS_SYSCLOCK); 

		addend = priv->default_addend;	
		config_addend(addend);

		/* updating the TC9560 time with present Host time*/
		getnstimeofday(&now);
		init_systime(now.tv_sec, now.tv_nsec);
	}
	return copy_to_user(ifr->ifr_data, &config, sizeof(config)) ?
		-EFAULT : 0;
}

/*!
 * \brief API to enable/disable an ancillary feature.
 *
 * \details This function is used to enable or disable an ancillary
 * device feature like PPS, PEROUT and EXTTS.
 *
 * \param[in] ptp – pointer to ptp_clock_info structure.
 * \param[in] rq – desired resource to enable or disable.
 * \param[in] on – caller passes one to enable or zero to disable.
 *
 * \return int
 *
 * \retval 0 on success and -ve(EINVAL or EOPNOTSUPP) number on failure.
 */

static int tc9560_ptp_enable(struct ptp_clock_info *ptp,
	struct ptp_clock_request *rq, int on)
{
	return -EOPNOTSUPP;
}


/*
 * structure describing a PTP hardware clock.
 */
static struct ptp_clock_info tc9560_ptp_clock_ops = {
    .owner = THIS_MODULE,
    .name = "DWC_ETH_QOS_clk",
    .max_adj = DWC_ETH_QOS_SYSCLOCK, /* the max possible frequency adjustment,
                            in parts per billion */
    .n_alarm = 0,   /* the number of programmable alarms */
    .n_ext_ts = 0,  /* the number of externel time stamp channels */
    .n_per_out = 0, /* the number of programmable periodic signals */
    .pps = 0,       /* indicates whether the clk supports a PPS callback */
    .adjfreq = tc9560_ptp_adjfreq,
    .adjtime = tc9560_ptp_adjtime,
    .gettime = tc9560_ptp_get_time,
    .settime = tc9560_ptp_set_time,
    .enable = tc9560_ptp_enable,
};



/*!
 * \brief API to register ptp clock driver.
 *
 * \details This function is used to register the ptp clock
 * driver to kernel. It also does some housekeeping work.
 *
 * \param[in] pdata – pointer to private data structure.
 *
 * \return int
 *
 * \retval 0 on success and -ve number on failure.
 */
int tc9560_ptp_init(struct tc9560_data *pdata)
{
    int ret = 0;
    DBGPR("-->ptp_init\n");

/*        if (!pdata->hw_feat.tsstssel) {
            ret = -1;
            pdata->ptp_clock = NULL;
            printk(KERN_ALERT "No PTP supports in HW\n"
                    "Aborting PTP clock driver registration\n");
            goto no_hw_ptp;
    }*/
    mutex_init (&pdata->ptp_mutex);

    pdata->ptp_clock_ops = tc9560_ptp_clock_ops;

    pdata->ptp_clock = ptp_clock_register(&pdata->ptp_clock_ops, &pdata->udev->intf->dev);

    if (IS_ERR(pdata->ptp_clock)) {
            pdata->ptp_clock = NULL;
            printk(KERN_ALERT "ptp_clock_register() failed\n");
    } else {
            DBGPR(KERN_ALERT "Added PTP HW clock successfully\n");
    }

    return ret;
}


/*!
 * \brief API to unregister ptp clock driver.
 *
 * \details This function is used to remove/unregister the ptp
 * clock driver from the kernel.
 *
 * \param[in] pdata – pointer to private data structure.
 *
 * \return void
 */
void tc9560_ptp_remove(struct tc9560_data *pdata)
{
    DBGPR("-->tc9560_ptp_remove\n");

    if (pdata->ptp_clock) {
        ptp_clock_unregister(pdata->ptp_clock);
        printk(KERN_ALERT "Removed PTP HW clock successfully\n");
    }

    DBGPR("<--tc9560_ptp_remove\n");
}

int tc9560_phc_index(struct tc9560_data *pdata)
{
    DBGPR("-->tc9560_phc_index\n");
    if (pdata->ptp_clock)
         return ptp_clock_index(pdata->ptp_clock);
    else
	return 0;
}


