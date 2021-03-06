/*
 * USB Network driver infrastructure
 * Copyright (C) 2000-2005 by David Brownell
 * Copyright (C) 2003-2005 David Hollis <dhollis@davehollis.com>
 * PROJECT: TC9560
 * Copyright (C) 2018  Toshiba Electronic Devices & Storage Corporation
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

/*
 * This is a generic "USB networking" framework that works with several
 * kinds of full and high speed networking devices:  host-to-host cables,
 * smart usb peripherals, and actual Ethernet adapters.
 *
 * These devices usually differ in terms of control protocols (if they
 * even have one!) and sometimes they define new framing to wrap or batch
 * Ethernet packets.  Otherwise, they talk to USB pretty much the same,
 * so interface (un)binding, endpoint I/O queues, fault handling, and other
 * issues can usefully be addressed by this framework.
 */

// #define	DEBUG			// error path messages, extra info
// #define	VERBOSE			// more; success messages

#include <linux/init.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/ctype.h>
#include <linux/ethtool.h>
#include <linux/workqueue.h>
#include <linux/mii.h>
#include <linux/usb.h>
#include <linux/usb/usbnet.h>
#include <linux/slab.h>
#include <linux/kernel.h>
#include <linux/pm_runtime.h>
#include "tc9560_common.h"

#define DRIVER_VERSION		"22-Aug-2005"

struct tc9560_usbnet {
  struct usbnet dev;
	/* added three additional queues */
	struct sk_buff_head txq_ptp;
	struct sk_buff_head txq_avba;
	struct sk_buff_head txq_avbb;
};

/* additional events */
#define EVENT_TX_AVBA_HALT    13
#define EVENT_TX_AVBB_HALT    14
#define EVENT_TX_PTP_HALT     15

/*-------------------------------------------------------------------------*/

/*
 * Nineteen USB 1.1 max size bulk transactions per frame (ms), max.
 * Several dozen bytes of IPv4 data can fit in two such transactions.
 * One maximum size Ethernet packet takes twenty four of them.
 * For high speed, each frame comfortably fits almost 36 max size
 * Ethernet packets (so queues should be bigger).
 *
 * The goal is to let the USB host controller be busy for 5msec or
 * more before an irq is required, under load.  Jumbograms change
 * the equation.
 */
//#define	MAX_QUEUE_MEMORY	(90 * 1518)
//#define	MAX_QUEUE_MEMORY	(60 * 20480)
#define	RX_QLEN(dev)		((dev)->rx_qlen)
#define	TX_QLEN(dev)		((dev)->tx_qlen)

// reawaken network queue this soon after stopping; else watchdog barks
#define TX_TIMEOUT_JIFFIES	(5*HZ)

/* throttle rx/tx briefly after some faults, so hub_wq might disconnect()
 * us (it polls at HZ/4 usually) before we report too many false errors.
 */
#define THROTTLE_JIFFIES	(HZ/8)

// between wakeups
#define UNLINK_TIMEOUT_MS	3

/*-------------------------------------------------------------------------*/

// randomly generated ethernet address
static u8	node_id [ETH_ALEN];

static const char driver_name [] = "usbnet";

/* use ethtool to change the level for any given device */
static int msg_level = -1;

/*-------------------------------------------------------------------------*/

static void intr_complete (struct urb *urb)
{
	struct usbnet	*dev = urb->context;
	int		status = urb->status;

	switch (status) {
	/* success */
	case 0:
		dev->driver_info->status(dev, urb);
		break;

	/* software-driven interface shutdown */
	case -ENOENT:		/* urb killed */
	case -ESHUTDOWN:	/* hardware gone */
		netif_dbg(dev, ifdown, dev->net,
			  "intr shutdown, code %d\n", status);
		return;

	/* NOTE:  not throttling like RX/TX, since this endpoint
	 * already polls infrequently
	 */
	default:
		netdev_dbg(dev->net, "intr status %d\n", status);
		break;
	}

	status = usb_submit_urb (urb, GFP_ATOMIC);
	if (status != 0)
		netif_err(dev, timer, dev->net,
			  "intr resubmit --> %d\n", status);
}

static int init_status (struct usbnet *dev, struct usb_interface *intf)
{
	char		*buf = NULL;
	unsigned	pipe = 0;
	unsigned	maxp;
	unsigned	period;

	if (!dev->driver_info->status)
		return 0;

	pipe = usb_rcvintpipe (dev->udev,
			dev->status->desc.bEndpointAddress
				& USB_ENDPOINT_NUMBER_MASK);
	maxp = usb_maxpacket (dev->udev, pipe, 0);

	/* avoid 1 msec chatter:  min 8 msec poll rate */
	period = max ((int) dev->status->desc.bInterval,
		(dev->udev->speed == USB_SPEED_HIGH) ? 7 : 3);

	buf = kmalloc (maxp, GFP_ATOMIC);
	if (buf) {
		dev->interrupt = usb_alloc_urb (0, GFP_ATOMIC);
		if (!dev->interrupt) {
			kfree (buf);
			return -ENOMEM;
		} else {
			usb_fill_int_urb(dev->interrupt, dev->udev, pipe,
				buf, maxp, intr_complete, dev, period);
			dev->interrupt->transfer_flags |= URB_FREE_BUFFER;
			dev_dbg(&intf->dev,
				"status ep%din, %d bytes period %d\n",
				usb_pipeendpoint(pipe), maxp, period);
		}
	}
	return 0;
}

/* The caller must hold list->lock */
static void __usbnet_queue_skb(struct sk_buff_head *list,
			struct sk_buff *newsk, enum skb_state state)
{
	struct skb_data *entry = (struct skb_data *) newsk->cb;

	__skb_queue_tail(list, newsk);
	entry->state = state;
}
/* some LK 2.4 HCDs oopsed if we freed or resubmitted urbs from
 * completion callbacks.  2.5 should have fixed those bugs...
 */

static enum skb_state defer_bh(struct usbnet *dev, struct sk_buff *skb,
		struct sk_buff_head *list, enum skb_state state)
{
	unsigned long		flags;
	enum skb_state 		old_state;
	struct skb_data *entry = (struct skb_data *) skb->cb;

	spin_lock_irqsave(&list->lock, flags);
	old_state = entry->state;
	entry->state = state;
	__skb_unlink(skb, list);
	spin_unlock(&list->lock);
	spin_lock(&dev->done.lock);
	__skb_queue_tail(&dev->done, skb);
	if (dev->done.qlen == 1)
		tasklet_schedule(&dev->bh);
	spin_unlock_irqrestore(&dev->done.lock, flags);
	return old_state;
}

/* some work can't be done in tasklets, so we use keventd
 *
 * NOTE:  annoying asymmetry:  if it's active, schedule_work() fails,
 * but tasklet_schedule() doesn't.  hope the failure is rare.
 */
void tc9560_usbnet_defer_kevent (struct usbnet *dev, int work)
{
	set_bit (work, &dev->flags);
	if (!schedule_work (&dev->kevent)) {
		if (net_ratelimit())
			netdev_err(dev->net, "kevent %d may have been dropped\n", work);
	} else {
		netdev_dbg(dev->net, "kevent %d scheduled\n", work);
	}
}


/*-------------------------------------------------------------------------*/

static void tc9560_rx_complete (struct urb *urb);

static int tc9560_rx_submit (struct usbnet *dev, struct urb *urb, gfp_t flags, unsigned int pipe)
{
	struct sk_buff		*skb;
	struct skb_data		*entry;
	int			retval = 0;
	unsigned long		lockflags;
	size_t			size = dev->rx_urb_size;
  unsigned    index = 0;
  struct tc9560_data *priv = NULL;

  priv = (struct tc9560_data *)dev->data[0];
  if (priv == NULL) {
		return -ENOLINK;
  }

  if (pipe == priv->avb_in) index = AVB_PIPE_IDX;
  else if (pipe == priv->avb_control) index = AVBCTL_PIPE_IDX;
  else if (pipe == priv->gptp_in) index = GPTP_PIPE_IDX;
  else  index = LEGACY_PIPE_IDX;

	/* prevent rx skb allocation when error ratio is high */
	if (test_bit(EVENT_RX_KILL, &dev->flags)) {
		usb_free_urb(urb);
		return -ENOLINK;
	}

	skb = __netdev_alloc_skb_ip_align(dev->net, size, flags);
	if (!skb) {
		netif_dbg(dev, rx_err, dev->net, "no rx skb\n");

    priv->kevent_pipe_flags |= (1 << index);
		tc9560_usbnet_defer_kevent (dev, EVENT_RX_MEMORY);
		usb_free_urb (urb);
		return -ENOMEM;
	}

	entry = (struct skb_data *) skb->cb;
	entry->urb = urb;
	entry->dev = dev;
	entry->length = 0;

	usb_fill_bulk_urb (urb, dev->udev, pipe,
		skb->data, size, tc9560_rx_complete, skb);

	spin_lock_irqsave (&dev->rxq.lock, lockflags);

	if (netif_running (dev->net) &&
	    netif_device_present (dev->net) &&
	    !test_bit (EVENT_RX_HALT, &dev->flags) &&
	    !test_bit (EVENT_DEV_ASLEEP, &dev->flags)) {
		switch (retval = usb_submit_urb (urb, GFP_ATOMIC)) {
		case -EPIPE:
			tc9560_usbnet_defer_kevent (dev, EVENT_RX_HALT);
			break;
		case -ENOMEM:
      priv->kevent_pipe_flags |= (1 << index);
			tc9560_usbnet_defer_kevent (dev, EVENT_RX_MEMORY);
			break;
		case -ENODEV:
			netif_dbg(dev, ifdown, dev->net, "device gone\n");
			netif_device_detach (dev->net);
			break;
		case -EHOSTUNREACH:
			retval = -ENOLINK;
			break;
		default:
			netif_dbg(dev, rx_err, dev->net,
				  "rx submit, %d\n", retval);
			tasklet_schedule (&dev->bh);
			break;
		case 0:
			__usbnet_queue_skb(&dev->rxq, skb, rx_start);
		}
	} else {
		netif_dbg(dev, ifdown, dev->net, "rx: stopped\n");
		retval = -ENOLINK;
	}
	spin_unlock_irqrestore (&dev->rxq.lock, lockflags);
	if (retval) {
		dev_kfree_skb_any (skb);
		usb_free_urb (urb);
	}
	return retval;
}


/*-------------------------------------------------------------------------*/

static inline void rx_process (struct usbnet *dev, struct sk_buff *skb)
{
	if (dev->driver_info->rx_fixup &&
	    !dev->driver_info->rx_fixup (dev, skb)) {
		/* With RX_ASSEMBLE, rx_fixup() must update counters */
		if (!(dev->driver_info->flags & FLAG_RX_ASSEMBLE))
			dev->net->stats.rx_errors++;
		goto done;
	}
	// else network stack removes extra byte if we forced a short packet

	/* all data was already cloned from skb inside the driver */
	if (dev->driver_info->flags & FLAG_MULTI_PACKET)
		goto done;

	if (skb->len < ETH_HLEN) {
		dev->net->stats.rx_errors++;
		dev->net->stats.rx_length_errors++;
		netif_dbg(dev, rx_err, dev->net, "rx length %d\n", skb->len);
	} else {
		usbnet_skb_return(dev, skb);
		return;
	}

done:
	skb_queue_tail(&dev->done, skb);
}

/*-------------------------------------------------------------------------*/

static void tc9560_rx_complete (struct urb *urb)
{
	struct sk_buff		*skb = (struct sk_buff *) urb->context;
	struct skb_data		*entry = (struct skb_data *) skb->cb;
	struct usbnet		*dev = entry->dev;
	int			urb_status = urb->status;
	enum skb_state		state;

	skb_put (skb, urb->actual_length);
	state = rx_done;
	entry->urb = NULL;

	switch (urb_status) {
	/* success */
	case 0:
		break;

	/* stalls need manual reset. this is rare ... except that
	 * when going through USB 2.0 TTs, unplug appears this way.
	 * we avoid the highspeed version of the ETIMEDOUT/EILSEQ
	 * storm, recovering as needed.
	 */
	case -EPIPE:
		dev->net->stats.rx_errors++;
		tc9560_usbnet_defer_kevent (dev, EVENT_RX_HALT);
		// FALLTHROUGH

	/* software-driven interface shutdown */
	case -ECONNRESET:		/* async unlink */
	case -ESHUTDOWN:		/* hardware gone */
		netif_dbg(dev, ifdown, dev->net,
			  "rx shutdown, code %d\n", urb_status);
		goto block;

	/* we get controller i/o faults during hub_wq disconnect() delays.
	 * throttle down resubmits, to avoid log floods; just temporarily,
	 * so we still recover when the fault isn't a hub_wq delay.
	 */
	case -EPROTO:
	case -ETIME:
	case -EILSEQ:
		dev->net->stats.rx_errors++;
		if (!timer_pending (&dev->delay)) {
			mod_timer (&dev->delay, jiffies + THROTTLE_JIFFIES);
			netif_dbg(dev, link, dev->net,
				  "rx throttle %d\n", urb_status);
		}
block:
		state = rx_cleanup;
		entry->urb = urb;
		urb = NULL;
		break;

	/* data overrun ... flush fifo? */
	case -EOVERFLOW:
		dev->net->stats.rx_over_errors++;
		// FALLTHROUGH

	default:
		state = rx_cleanup;
		dev->net->stats.rx_errors++;
		netif_dbg(dev, rx_err, dev->net, "rx status %d\n", urb_status);
		break;
	}

	/* stop rx if packet error rate is high */
	if (++dev->pkt_cnt > 30) {
		dev->pkt_cnt = 0;
		dev->pkt_err = 0;
	} else {
		if (state == rx_cleanup)
			dev->pkt_err++;
		if (dev->pkt_err > 20)
			set_bit(EVENT_RX_KILL, &dev->flags);
	}

	state = defer_bh(dev, skb, &dev->rxq, state);

	if (urb) {
		if (netif_running (dev->net) &&
		    !test_bit (EVENT_RX_HALT, &dev->flags) &&
		    state != unlink_start) {
			tc9560_rx_submit (dev, urb, GFP_ATOMIC, urb->pipe);
			usb_mark_last_busy(dev->udev);
			return;
		}
		usb_free_urb (urb);
	}
	netif_dbg(dev, rx_err, dev->net, "no read resubmitted\n");
}



static int unlink_urbs (struct usbnet *dev, struct sk_buff_head *q)
{
	unsigned long		flags;
	struct sk_buff		*skb;
	int			count = 0;

	spin_lock_irqsave (&q->lock, flags);
	while (!skb_queue_empty(q)) {
		struct skb_data		*entry;
		struct urb		*urb;
		int			retval;

		skb_queue_walk(q, skb) {
			entry = (struct skb_data *) skb->cb;
			if (entry->state != unlink_start)
				goto found;
		}
		break;
found:
		entry->state = unlink_start;
		urb = entry->urb;

		/*
		 * Get reference count of the URB to avoid it to be
		 * freed during usb_unlink_urb, which may trigger
		 * use-after-free problem inside usb_unlink_urb since
		 * usb_unlink_urb is always racing with .complete
		 * handler(include defer_bh).
		 */
		usb_get_urb(urb);
		spin_unlock_irqrestore(&q->lock, flags);
		// during some PM-driven resume scenarios,
		// these (async) unlinks complete immediately
		retval = usb_unlink_urb (urb);
		if (retval != -EINPROGRESS && retval != 0)
			netdev_dbg(dev->net, "unlink urb err, %d\n", retval);
		else
			count++;
		usb_put_urb(urb);
		spin_lock_irqsave(&q->lock, flags);
	}
	spin_unlock_irqrestore (&q->lock, flags);
	return count;
}

/*-------------------------------------------------------------------------*/

/* precondition: never called in_interrupt */
static void usbnet_terminate_urbs(struct tc9560_usbnet *pdata)
{
	struct usbnet *dev;
	DECLARE_WAITQUEUE(wait, current);
	int temp;

	dev = &(pdata->dev);

	/* ensure there are no more active urbs */
	add_wait_queue(&dev->wait, &wait);
	set_current_state(TASK_UNINTERRUPTIBLE);
	temp = unlink_urbs(dev, &dev->txq) +
				 unlink_urbs(dev, &pdata->txq_ptp) + 
				 unlink_urbs(dev, &pdata->txq_avba) + 
				 unlink_urbs(dev, &pdata->txq_avbb) + 
				 unlink_urbs(dev, &dev->rxq);

	/* maybe wait for deletions to finish. */
	while (!skb_queue_empty(&dev->rxq)
		&& !skb_queue_empty(&dev->txq)
		&& !skb_queue_empty(&pdata->txq_ptp)
		&& !skb_queue_empty(&pdata->txq_avba)
		&& !skb_queue_empty(&pdata->txq_avbb)
		&& !skb_queue_empty(&dev->done)) {
			schedule_timeout(msecs_to_jiffies(UNLINK_TIMEOUT_MS));
			set_current_state(TASK_UNINTERRUPTIBLE);
			netif_dbg(dev, ifdown, dev->net,
				  "waited for %d urb completions\n", temp);
	}
	set_current_state(TASK_RUNNING);
	remove_wait_queue(&dev->wait, &wait);
}

int tc9560_usbnet_stop (struct net_device *net)
{
	struct tc9560_usbnet		*pdata = netdev_priv(net);
	struct usbnet		*dev;
	struct driver_info	*info;
	int			retval, pm;

	dev = &(pdata->dev);
	info = dev->driver_info;

	clear_bit(EVENT_DEV_OPEN, &dev->flags);
	netif_stop_queue (net);

	netif_info(dev, ifdown, dev->net,
		   "stop stats: rx/tx %lu/%lu, errs %lu/%lu\n",
		   net->stats.rx_packets, net->stats.tx_packets,
		   net->stats.rx_errors, net->stats.tx_errors);

	/* to not race resume */
	pm = usb_autopm_get_interface(dev->intf);
	/* allow minidriver to stop correctly (wireless devices to turn off
	 * radio etc) */
	if (info->stop) {
		retval = info->stop(dev);
		if (retval < 0)
			netif_info(dev, ifdown, dev->net,
				   "stop fail (%d) usbnet usb-%s-%s, %s\n",
				   retval,
				   dev->udev->bus->bus_name, dev->udev->devpath,
				   info->description);
	}

	if (!(info->flags & FLAG_AVOID_UNLINK_URBS))
		usbnet_terminate_urbs(pdata);

	usbnet_status_stop(dev);

	usbnet_purge_paused_rxq(dev);

	/* deferred work (task, timer, softirq) must also stop.
	 * can't flush_scheduled_work() until we drop rtnl (later),
	 * else workers could deadlock; so make workers a NOP.
	 */
	dev->flags = 0;
	del_timer_sync (&dev->delay);
	tasklet_kill (&dev->bh);
	if (!pm)
		usb_autopm_put_interface(dev->intf);

	if (info->manage_power &&
	    !test_and_clear_bit(EVENT_NO_RUNTIME_PM, &dev->flags))
		info->manage_power(dev, 0);
	else
		usb_autopm_put_interface(dev->intf);

	return 0;
}

/*-------------------------------------------------------------------------*/

/* posts reads, and enables write queuing
   precondition: never called in_interrupt */

int tc9560_usbnet_open (struct net_device *net)
{
	struct tc9560_usbnet		*pdata = netdev_priv(net);
	struct usbnet		*dev;
	int			retval;
	struct driver_info	*info;

	dev = &(pdata->dev);
	info = dev->driver_info;

	if ((retval = usb_autopm_get_interface(dev->intf)) < 0) {
		netif_info(dev, ifup, dev->net,
			   "resumption fail (%d) usbnet usb-%s-%s, %s\n",
			   retval,
			   dev->udev->bus->bus_name,
			   dev->udev->devpath,
			   info->description);
		goto done_nopm;
	}

	/* put into "known safe" state */
	if (info->reset && (retval = info->reset (dev)) < 0) {
		netif_info(dev, ifup, dev->net,
			   "open reset fail (%d) usbnet usb-%s-%s, %s\n",
			   retval,
			   dev->udev->bus->bus_name,
			   dev->udev->devpath,
			   info->description);
		goto done;
	}

	/* hard_mtu or rx_urb_size may change in reset() */
	usbnet_update_max_qlen(dev);

	/* insist peer be connected */
	if (info->check_connect && (retval = info->check_connect (dev)) < 0) {
		netif_dbg(dev, ifup, dev->net, "can't open; %d\n", retval);
		goto done;
	}

	/* start any status interrupt transfer */
	if (dev->interrupt) {
		retval = usbnet_status_start(dev, GFP_KERNEL);
		if (retval < 0) {
			netif_err(dev, ifup, dev->net,
				  "intr submit %d\n", retval);
			goto done;
		}
	}

	set_bit(EVENT_DEV_OPEN, &dev->flags);
	netif_start_queue (net);
	netif_info(dev, ifup, dev->net,
		   "open: enable queueing (rx %d, tx %d) mtu %d %s framing\n",
		   (int)RX_QLEN(dev), (int)TX_QLEN(dev),
		   dev->net->mtu,
		   (dev->driver_info->flags & FLAG_FRAMING_NC) ? "NetChip" :
		   (dev->driver_info->flags & FLAG_FRAMING_GL) ? "GeneSys" :
		   (dev->driver_info->flags & FLAG_FRAMING_Z) ? "Zaurus" :
		   (dev->driver_info->flags & FLAG_FRAMING_RN) ? "RNDIS" :
		   (dev->driver_info->flags & FLAG_FRAMING_AX) ? "ASIX" :
		   "simple");

	/* reset rx error state */
	dev->pkt_cnt = 0;
	dev->pkt_err = 0;
	clear_bit(EVENT_RX_KILL, &dev->flags);

	/* delay posting reads until we're fully open */
	tasklet_schedule (&dev->bh);
	if (info->manage_power) {
		retval = info->manage_power(dev, 1);
		if (retval < 0) {
			retval = 0;
			set_bit(EVENT_NO_RUNTIME_PM, &dev->flags);
		} else {
			usb_autopm_put_interface(dev->intf);
		}
	}
	return retval;
done:
	usb_autopm_put_interface(dev->intf);
done_nopm:
	return retval;
}

/* drivers may override default ethtool_ops in their bind() routine */
static const struct ethtool_ops usbnet_ethtool_ops = {
	.get_settings		= usbnet_get_settings,
	.set_settings		= usbnet_set_settings,
	.get_link		= usbnet_get_link,
	.nway_reset		= usbnet_nway_reset,
	.get_drvinfo		= usbnet_get_drvinfo,
	.get_msglevel		= usbnet_get_msglevel,
	.set_msglevel		= usbnet_set_msglevel,
	.get_ts_info		= ethtool_op_get_ts_info,
};

/*-------------------------------------------------------------------------*/

static void __handle_link_change(struct usbnet *dev)
{
	if (!test_bit(EVENT_DEV_OPEN, &dev->flags))
		return;

	if (!netif_carrier_ok(dev->net)) {
		/* kill URBs for reading packets to save bus bandwidth */
		unlink_urbs(dev, &dev->rxq);

		/*
		 * tx_timeout will unlink URBs for sending packets and
		 * tx queue is stopped by netcore after link becomes off
		 */
	} else {
		/* submitting URBs for reading packets */
		tasklet_schedule(&dev->bh);
	}

	/* hard_mtu or rx_urb_size may change during link change */
	usbnet_update_max_qlen(dev);

	clear_bit(EVENT_LINK_CHANGE, &dev->flags);
}

static void usbnet_set_rx_mode(struct net_device *net)
{
	struct tc9560_usbnet		*pdata = netdev_priv(net);
	struct usbnet		*dev;

	dev = &(pdata->dev);

	tc9560_usbnet_defer_kevent(dev, EVENT_SET_RX_MODE);
}

static void __handle_set_rx_mode(struct usbnet *dev)
{
	if (dev->driver_info->set_rx_mode)
		(dev->driver_info->set_rx_mode)(dev);

	clear_bit(EVENT_SET_RX_MODE, &dev->flags);
}

/* work that cannot be done in interrupt context uses keventd.
 *
 * NOTE:  with 2.5 we could do more of this using completion callbacks,
 * especially now that control transfers can be queued.
 */
static void
kevent (struct work_struct *work)
{
	struct usbnet		*dev =
		container_of(work, struct usbnet, kevent);
	int			status;
	struct tc9560_usbnet		*pdata = NULL;
	struct tc9560_data *priv = NULL;

	priv = (struct tc9560_data *)dev->data[0];
	if (priv == NULL) {
		return;
	}

	if (dev) {
		pdata = netdev_priv(dev->net);
		return;
	}

	/* usb_clear_halt() needs a thread context */
	if (test_bit (EVENT_TX_HALT, &dev->flags)) {
		unlink_urbs (dev, &dev->txq);
		status = usb_autopm_get_interface(dev->intf);
		if (status < 0)
			goto fail_pipe_tx;
		status = usb_clear_halt (dev->udev, priv->legacy_out);
		usb_autopm_put_interface(dev->intf);
		if (status < 0 &&
		    status != -EPIPE &&
		    status != -ESHUTDOWN) {
			if (netif_msg_tx_err (dev))
fail_pipe_tx:
				netdev_err(dev->net, "can't clear tx halt, status %d\n",
					   status);
		} else {
			clear_bit (EVENT_TX_HALT, &dev->flags);
			if (status != -ESHUTDOWN)
				netif_wake_subqueue (dev->net, TC9560_TX_LEG);
		}
	}
	if (test_bit (EVENT_TX_AVBA_HALT, &dev->flags)) {
		unlink_urbs (dev, &pdata->txq_avba);
		status = usb_autopm_get_interface(dev->intf);
		if (status < 0)
			goto fail_pipe_tx_avb;
		status = usb_clear_halt (dev->udev, priv->avb_a_out);
		usb_autopm_put_interface(dev->intf);
		if (status < 0 &&
		    status != -EPIPE &&
		    status != -ESHUTDOWN) {
			if (netif_msg_tx_err (dev))
fail_pipe_tx_avb:
				netdev_err(dev->net, "can't clear tx halt, status %d\n",
					   status);
		} else {
			clear_bit (EVENT_TX_AVBA_HALT, &dev->flags);
			if (status != -ESHUTDOWN)
				netif_wake_subqueue (dev->net, TC9560_TX_AVBA);
		}
	}
	if (test_bit (EVENT_TX_AVBB_HALT, &dev->flags)) {
		unlink_urbs (dev, &pdata->txq_avbb);
		status = usb_autopm_get_interface(dev->intf);
		if (status < 0)
			goto fail_pipe_tx_avbc;
		status = usb_clear_halt (dev->udev, priv->avb_b_out);
		usb_autopm_put_interface(dev->intf);
		if (status < 0 &&
		    status != -EPIPE &&
		    status != -ESHUTDOWN) {
			if (netif_msg_tx_err (dev))
fail_pipe_tx_avbc:
				netdev_err(dev->net, "can't clear tx halt, status %d\n",
					   status);
		} else {
			clear_bit (EVENT_TX_AVBB_HALT, &dev->flags);
			if (status != -ESHUTDOWN)
				netif_wake_subqueue (dev->net, TC9560_TX_AVBB);
		}
	}
	if (test_bit (EVENT_TX_PTP_HALT, &dev->flags)) {
		unlink_urbs (dev, &pdata->txq_ptp);
		status = usb_autopm_get_interface(dev->intf);
		if (status < 0)
			goto fail_pipe_tx_ptp;
		status = usb_clear_halt (dev->udev, priv->gptp_out);
		usb_autopm_put_interface(dev->intf);
		if (status < 0 &&
		    status != -EPIPE &&
		    status != -ESHUTDOWN) {
			if (netif_msg_tx_err (dev))
fail_pipe_tx_ptp:
				netdev_err(dev->net, "can't clear tx halt, status %d\n",
					   status);
		} else {
			clear_bit (EVENT_TX_PTP_HALT, &dev->flags);
			if (status != -ESHUTDOWN)
				netif_wake_subqueue (dev->net, TC9560_TX_PTP);
		}
	}
	if (test_bit (EVENT_RX_HALT, &dev->flags)) {
		unlink_urbs (dev, &dev->rxq);
		status = usb_autopm_get_interface(dev->intf);
		if (status < 0)
			goto fail_halt;
		status = usb_clear_halt (dev->udev, dev->in);
		usb_autopm_put_interface(dev->intf);
		if (status < 0 &&
		    status != -EPIPE &&
		    status != -ESHUTDOWN) {
			if (netif_msg_rx_err (dev))
fail_halt:
				netdev_err(dev->net, "can't clear rx halt, status %d\n",
					   status);
		} else {
			clear_bit (EVENT_RX_HALT, &dev->flags);
			tasklet_schedule (&dev->bh);
		}
	}

	/* tasklet could resubmit itself forever if memory is tight */
	if (test_bit (EVENT_RX_MEMORY, &dev->flags)) {
		struct urb	*urb = NULL;
		int resched = 1;
    int j;
    unsigned pipe[4];
    struct tc9560_data *priv = NULL;

    priv = (struct tc9560_data *)dev->data[0];
    if (priv == NULL) return; 

    pipe[LEGACY_PIPE_IDX] = priv->legacy_in;
    pipe[AVB_PIPE_IDX]    = priv->avb_in;
    pipe[AVBCTL_PIPE_IDX] = priv->avb_control;
    pipe[GPTP_PIPE_IDX]   = priv->gptp_in;

    for (j = 0; j < 4; j++) {
      if((priv->kevent_pipe_flags & (1 << j)) == 0) continue;
      
      priv->kevent_pipe_flags &= ~(1 << j);
		  if (netif_running (dev->net))
		  	urb = usb_alloc_urb (0, GFP_ATOMIC);
		  else
		  	clear_bit (EVENT_RX_MEMORY, &dev->flags);
		  if (urb != NULL) {
		  	clear_bit (EVENT_RX_MEMORY, &dev->flags);
		  	status = usb_autopm_get_interface(dev->intf);
		  	if (status < 0) {
		  		usb_free_urb(urb);
		  		goto fail_lowmem;
		  	}

		  	if (tc9560_rx_submit (dev, urb, GFP_ATOMIC, pipe[j]) == -ENOLINK)
		  		resched = 0;
		  	usb_autopm_put_interface(dev->intf);
fail_lowmem:
		  	if (resched)
		  		tasklet_schedule (&dev->bh);
		  }
    }
	}

	if (test_bit (EVENT_LINK_RESET, &dev->flags)) {
		struct driver_info	*info = dev->driver_info;
		int			retval = 0;

		clear_bit (EVENT_LINK_RESET, &dev->flags);
		status = usb_autopm_get_interface(dev->intf);
		if (status < 0)
			goto skip_reset;
		if(info->link_reset && (retval = info->link_reset(dev)) < 0) {
			usb_autopm_put_interface(dev->intf);
skip_reset:
			netdev_info(dev->net, "link reset failed (%d) usbnet usb-%s-%s, %s\n",
				    retval,
				    dev->udev->bus->bus_name,
				    dev->udev->devpath,
				    info->description);
		} else {
			usb_autopm_put_interface(dev->intf);
		}

		/* handle link change from link resetting */
		__handle_link_change(dev);
	}

	if (test_bit (EVENT_LINK_CHANGE, &dev->flags))
		__handle_link_change(dev);

	if (test_bit (EVENT_SET_RX_MODE, &dev->flags))
		__handle_set_rx_mode(dev);


	if (dev->flags)
		netdev_dbg(dev->net, "kevent done, flags = 0x%lx\n", dev->flags);
}
/*-------------------------------------------------------------------------*/

static void tx_complete (struct urb *urb)
{
  struct sk_buff    *skb = (struct sk_buff *) urb->context;
  struct skb_data   *entry = (struct skb_data *) skb->cb;
  struct usbnet   *dev = entry->dev;
	int idx = skb_get_queue_mapping(skb);
	struct tc9560_usbnet		*pdata = netdev_priv(dev->net);

  if (urb->status == 0) {
    dev->net->stats.tx_packets += entry->packets;
    dev->net->stats.tx_bytes += entry->length;
  } else {
    dev->net->stats.tx_errors++;

    switch (urb->status) {
    case -EPIPE:
			#if 1 // for MQS
			if(idx == TC9560_TX_AVBA)
			{
      	tc9560_usbnet_defer_kevent (dev, EVENT_TX_AVBA_HALT);
			}
			else if (idx == TC9560_TX_AVBB) 
			{
      	tc9560_usbnet_defer_kevent (dev, EVENT_TX_AVBB_HALT);
			}
			else if (idx == TC9560_TX_PTP) 
			{
      	tc9560_usbnet_defer_kevent (dev, EVENT_TX_PTP_HALT);
			}
			else 
			{
      	tc9560_usbnet_defer_kevent (dev, EVENT_TX_HALT);
			}
			#else
      tc9560_usbnet_defer_kevent (dev, EVENT_TX_HALT);
			#endif
      break;

    /* software-driven interface shutdown */
    case -ECONNRESET:   /* async unlink */
    case -ESHUTDOWN:    /* hardware gone */
      break;

		/* like rx, tx gets controller i/o faults during hub_wq
		 * delays and so it uses the same throttling mechanism.
		 */
		case -EPROTO:
		case -ETIME:
		case -EILSEQ:
			usb_mark_last_busy(dev->udev);
			if (!timer_pending (&dev->delay)) {
				mod_timer (&dev->delay,
					jiffies + THROTTLE_JIFFIES);
				netif_dbg(dev, link, dev->net,
					  "tx throttle %d\n", urb->status);
			}
			#if 1 /* for MQS */
			{
				netif_stop_subqueue (dev->net, idx);
			}
			#else
			netif_stop_queue (dev->net);
			#endif
			break;
		default:
			netif_dbg(dev, tx_err, dev->net,
				  "tx err %d\n", entry->urb->status);
			break;
		}
	}

	usb_autopm_put_interface_async(dev->intf);
	#if 1 /* for MQS */
	if(idx == TC9560_TX_AVBA)
	{
		(void) defer_bh(dev, skb, &pdata->txq_avba, tx_done);
	}
	else if (idx == TC9560_TX_AVBB) 
	{
		(void) defer_bh(dev, skb, &pdata->txq_avbb, tx_done);
	}
	else if (idx == TC9560_TX_PTP) 
	{
		(void) defer_bh(dev, skb, &pdata->txq_ptp, tx_done);
	}
	else 
	{
		(void) defer_bh(dev, skb, &dev->txq, tx_done);
	}
	#else
	(void) defer_bh(dev, skb, &dev->txq, tx_done);
	#endif
}

/*-------------------------------------------------------------------------*/

void tc9560_usbnet_tx_timeout (struct net_device *net)
{
	struct tc9560_usbnet		*pdata = netdev_priv(net);
	struct usbnet		*dev;
	dev = &(pdata->dev);

	/* IF the upper layer can pass the timeout queue information,
	 * then we only need unlink that one queue */
	unlink_urbs (dev, &dev->txq);  
	unlink_urbs (dev, &pdata->txq_avba);  
	unlink_urbs (dev, &pdata->txq_avbb);  
	unlink_urbs (dev, &pdata->txq_ptp);  
	tasklet_schedule (&dev->bh);
	/* this needs to be handled individually because the generic layer
	 * doesn't know what is sufficient and could not restore private
	 * information if a remedy of an unconditional reset were used.
	 */
	if (dev->driver_info->recover)
		(dev->driver_info->recover)(dev);
}

/*-------------------------------------------------------------------------*/

static int build_dma_sg(const struct sk_buff *skb, struct urb *urb)
{
  unsigned num_sgs, total_len = 0;
  int i, s = 0;

  num_sgs = skb_shinfo(skb)->nr_frags + 1;
  if (num_sgs == 1)
    return 0;

  /* reserve one for zero packet */
  urb->sg = kmalloc((num_sgs + 1) * sizeof(struct scatterlist),
        GFP_ATOMIC);
  if (!urb->sg)
    return -ENOMEM;

  urb->num_sgs = num_sgs;
  sg_init_table(urb->sg, urb->num_sgs + 1);

  sg_set_buf(&urb->sg[s++], skb->data, skb_headlen(skb));
  total_len += skb_headlen(skb);

  for (i = 0; i < skb_shinfo(skb)->nr_frags; i++) {
    struct skb_frag_struct *f = &skb_shinfo(skb)->frags[i];

    total_len += skb_frag_size(f);
    sg_set_page(&urb->sg[i + s], f->page.p, f->size,
        f->page_offset);
  }
  urb->transfer_buffer_length = total_len;

  return 1;
}

netdev_tx_t tc9560_usbnet_start_xmit (struct sk_buff *skb,
             struct net_device *net)
{
	struct tc9560_usbnet		*pdata = netdev_priv(net);
	struct usbnet				*dev;
  int     length;
  struct urb    *urb = NULL;
  struct skb_data   *entry;
  struct driver_info  *info;
  unsigned long   flags;
  int retval;
  int outIdx = 0, outPipe;
  struct tc9560_data *priv = NULL;

  dev = &(pdata->dev);
  info = dev->driver_info;
  priv = (struct tc9560_data *)dev->data[0];

  if (skb)
    skb_tx_timestamp(skb);

  // some devices want funky USB-level framing, for
  // win32 driver (usually) and/or hardware quirks
  if (info->tx_fixup) {
    skb = info->tx_fixup (dev, skb, GFP_ATOMIC);
    if (!skb) {
      /* packet collected; minidriver waiting for more */
      if (info->flags & FLAG_MULTI_PACKET)
        goto not_drop;
      netif_dbg(dev, tx_err, dev->net, "can't tx_fixup skb\n");
      goto drop;
    }
  }

  if (!(urb = usb_alloc_urb (0, GFP_ATOMIC))) {
    netif_dbg(dev, tx_err, dev->net, "no urb\n");
    goto drop;
  }

  entry = (struct skb_data *) skb->cb;
  entry->urb = urb;
  entry->dev = dev;

 	outIdx = skb_get_queue_mapping(skb);
	if(outIdx == TC9560_TX_AVBA) 
	{
		outPipe = priv->avb_a_out; 
	}
	else if (outIdx == TC9560_TX_AVBB)  outPipe = priv->avb_b_out; 
	else if (outIdx == TC9560_TX_PTP) 
	{
		outPipe = priv->gptp_out; 
	}
	else  outPipe = priv->legacy_out;
 
  usb_fill_bulk_urb (urb, dev->udev, outPipe,
      skb->data, skb->len, tx_complete, skb);
 if (dev->can_dma_sg) {
    if (build_dma_sg(skb, urb) < 0)
      goto drop;
  }
  length = urb->transfer_buffer_length;

  /* don't assume the hardware handles USB_ZERO_PACKET
   * NOTE:  strictly conforming cdc-ether devices should expect
   * the ZLP here, but ignore the one-byte packet.
   * NOTE2: CDC NCM specification is different from CDC ECM when
   * handling ZLP/short packets, so cdc_ncm driver will make short
   * packet itself if needed.
   */
  if (length % dev->maxpacket == 0) {
    if (!(info->flags & FLAG_SEND_ZLP)) {
      if (!(info->flags & FLAG_MULTI_PACKET)) {
        length++;
        if (skb_tailroom(skb) && !urb->num_sgs) {
          skb->data[skb->len] = 0;
          __skb_put(skb, 1);
        } else if (urb->num_sgs)
          sg_set_buf(&urb->sg[urb->num_sgs++],
              dev->padding_pkt, 1);
      }
    } else
      urb->transfer_flags |= URB_ZERO_PACKET;
  }
  urb->transfer_buffer_length = length;

	if (info->flags & FLAG_MULTI_PACKET) {
		/* Driver has set number of packets and a length delta.
		 * Calculate the complete length and ensure that it's
		 * positive.
		 */
		entry->length += length;
		if (WARN_ON_ONCE(entry->length <= 0))
			entry->length = length;
	} else {
		usbnet_set_skb_tx_stats(skb, 1, length);
	}

	if(outIdx == TC9560_TX_AVBA)
	{
		spin_lock_irqsave(&pdata->txq_avba.lock, flags);
	}
	else if (outIdx == TC9560_TX_AVBB)
	{
		spin_lock_irqsave(&pdata->txq_avbb.lock, flags);
	}
	else if (outIdx == TC9560_TX_PTP) 
	{
		spin_lock_irqsave(&pdata->txq_ptp.lock, flags);
	}
	else 
	{
		spin_lock_irqsave(&dev->txq.lock, flags);
	}
	retval = usb_autopm_get_interface_async(dev->intf);
	if (retval < 0) {
		if(outIdx == TC9560_TX_AVBA)
		{
			spin_unlock_irqrestore(&pdata->txq_avba.lock, flags);
		}
		else if (outIdx == TC9560_TX_AVBB)
		{
			spin_unlock_irqrestore(&pdata->txq_avbb.lock, flags);
		}
		else if (outIdx == TC9560_TX_PTP) 
		{
			spin_unlock_irqrestore(&pdata->txq_ptp.lock, flags);
		}
		else 
		{
			spin_unlock_irqrestore(&dev->txq.lock, flags);
		}
		goto drop;
	}

#ifdef CONFIG_PM
  /* if this triggers the device is still a sleep */
  if (test_bit(EVENT_DEV_ASLEEP, &dev->flags)) {
    /* transmission will be done in resume */
    usb_anchor_urb(urb, &dev->deferred);
    /* no use to process more packets */
    netif_stop_subqueue(net, outIdx);
    usb_put_urb(urb);
		if(outIdx == TC9560_TX_AVBA)
		{
			spin_unlock_irqrestore(&pdata->txq_avba.lock, flags);
		}
		else if (outIdx == TC9560_TX_AVBB)
		{
			spin_unlock_irqrestore(&pdata->txq_avbb.lock, flags);
		}
		else if (outIdx == TC9560_TX_PTP) 
		{
			spin_unlock_irqrestore(&pdata->txq_ptp.lock, flags);
		}
		else 
		{
			spin_unlock_irqrestore(&dev->txq.lock, flags);
		}
    netdev_dbg(dev->net, "Delaying transmission for resumption\n");
    goto deferred;
  }
#endif

  switch ((retval = usb_submit_urb (urb, GFP_ATOMIC))) {
  case -EPIPE:
	#if 1 /* for MQS */
    netif_stop_subqueue (net, outIdx);
		if(outIdx == TC9560_TX_AVBA)
		{
    	tc9560_usbnet_defer_kevent (dev, EVENT_TX_AVBA_HALT);
		}
		else if (outIdx == TC9560_TX_AVBB)
		{
    	tc9560_usbnet_defer_kevent (dev, EVENT_TX_AVBB_HALT);
		}
		else if (outIdx == TC9560_TX_PTP) 
		{
    	tc9560_usbnet_defer_kevent (dev, EVENT_TX_PTP_HALT);
		}
		else 
		{
    	tc9560_usbnet_defer_kevent (dev, EVENT_TX_HALT);
		}
	#else
    netif_stop_queue (net);
    tc9560_usbnet_defer_kevent (dev, EVENT_TX_HALT);
	#endif
    usb_autopm_put_interface_async(dev->intf);
    break;
  default:
    usb_autopm_put_interface_async(dev->intf);
    netif_dbg(dev, tx_err, dev->net,
        "tx: submit urb err %d\n", retval);
    break;
  case 0:
    net->trans_start = jiffies;
	#if 1 /* for MQS */
		if(outIdx == TC9560_TX_AVBA)
		{
    	__usbnet_queue_skb(&pdata->txq_avba, skb, tx_start);
    	if (pdata->txq_avba.qlen >= (TX_QLEN (dev)/4 )) 
			{
      	netif_stop_subqueue (net, TC9560_TX_AVBA);
			}
		}
		else if (outIdx == TC9560_TX_AVBB)   
		{
    	__usbnet_queue_skb(&pdata->txq_avbb, skb, tx_start);
    	if (pdata->txq_avbb.qlen >= TX_QLEN (dev))
			{
      	netif_stop_subqueue (net, TC9560_TX_AVBB);
			}
		}
		else if (outIdx == TC9560_TX_PTP)   
		{
    	__usbnet_queue_skb(&pdata->txq_ptp, skb, tx_start);
    	if (pdata->txq_ptp.qlen >= TX_QLEN (dev))
			{
      	netif_stop_subqueue (net, TC9560_TX_PTP);
			}
		}
		else { 
    	__usbnet_queue_skb(&dev->txq, skb, tx_start);
    	if (dev->txq.qlen >= TX_QLEN (dev))
			{
      	netif_stop_subqueue (net, TC9560_TX_LEG);
			}
		}
	#else
    __usbnet_queue_skb(&dev->txq, skb, tx_start);
    if (dev->txq.qlen >= TX_QLEN (dev))
      netif_stop_queue (net);
	#endif
  }

	if(outIdx == TC9560_TX_AVBA)
	{
		spin_unlock_irqrestore(&pdata->txq_avba.lock, flags);
	}
	else if (outIdx == TC9560_TX_AVBB)
	{
		spin_unlock_irqrestore(&pdata->txq_avbb.lock, flags);
	}
	else if (outIdx == TC9560_TX_PTP) 
	{
		spin_unlock_irqrestore(&pdata->txq_ptp.lock, flags);
	}
	else 
	{
		spin_unlock_irqrestore(&dev->txq.lock, flags);
	}

  if (retval) {
    netif_dbg(dev, tx_err, dev->net, "drop, code %d\n", retval);
drop:
    dev->net->stats.tx_dropped++;
not_drop:
    if (skb)
      dev_kfree_skb_any (skb);
    if (urb) {
      kfree(urb->sg);
      usb_free_urb(urb);
    }
  } else
    netif_dbg(dev, tx_queued, dev->net,
        "> tx, len %d, type 0x%x\n", length, skb->protocol);
#ifdef CONFIG_PM
deferred:
#endif
  return NETDEV_TX_OK;
}


static int rx_alloc_submit(struct usbnet *dev, gfp_t flags)
{
	struct urb	*urb;
	int		i;
	int		ret = 0, error = 0;
  struct tc9560_data *priv = NULL;

  priv = (struct tc9560_data *)dev->data[0];
  if (priv == NULL) {
			ret = -ENOMEM;
			goto err;
  }

	/* don't refill the queue all at once */
	for (i = 0; i < 4*20 && dev->rxq.qlen < 4 * RX_QLEN(dev); i+=4) {
    /* legacy pipe */
		urb = usb_alloc_urb(0, flags);
		if (urb != NULL) {
      urb->pipe = priv->legacy_in;
			ret = tc9560_rx_submit(dev, urb, flags, urb->pipe);
			if (ret) {
				error |= ret; // goto err;
      }
		} else {
			error = -ENOMEM;
			goto err;
		}

    /* gptp pipe */
		urb = usb_alloc_urb(0, flags);
		if (urb != NULL) {
      urb->pipe = priv->gptp_in;
			ret = tc9560_rx_submit(dev, urb, flags, urb->pipe);
			if (ret)
				error |= ret; // goto err;
		} else {
			error = -ENOMEM;
			goto err;
		}

    /* avb pipe */
		urb = usb_alloc_urb(0, flags);
		if (urb != NULL) {
      urb->pipe = priv->avb_in;
			ret = tc9560_rx_submit(dev, urb, flags, urb->pipe);
			if (ret)
				error |= ret; // goto err;
        if (dev->rxq.qlen == 0) goto err;
		} else {
			error = -ENOMEM;
			goto err;
		}

    /* avb_ctl pipe */
		urb = usb_alloc_urb(0, flags);
		if (urb != NULL) {
      urb->pipe = priv->avb_control;
			ret = tc9560_rx_submit(dev, urb, flags, urb->pipe);
			if (ret)
				error |= ret; // goto err;
		} else {
			error = -ENOMEM;
			goto err;
		}
	}

err:
	return error;
}

/*-------------------------------------------------------------------------*/

// tasklet (work deferred from completions, in_irq) or timer

static void usbnet_bh (unsigned long param)
{
	struct tc9560_usbnet	*pdata = (struct tc9560_usbnet *) param;
	struct usbnet		  *dev = &(pdata->dev);
	struct sk_buff		*skb;
	struct skb_data		*entry;

	while ((skb = skb_dequeue (&dev->done))) {
		entry = (struct skb_data *) skb->cb;
		switch (entry->state) {
		case rx_done:
			entry->state = rx_cleanup;
			rx_process (dev, skb);
			continue;
		case tx_done:
			kfree(entry->urb->sg);
		case rx_cleanup:
			usb_free_urb (entry->urb);
			dev_kfree_skb (skb);
			continue;
		default:
			netdev_dbg(dev->net, "bogus skb state %d\n", entry->state);
		}
	}

	/* restart RX again after disabling due to high error rate */
	clear_bit(EVENT_RX_KILL, &dev->flags);

	/* waiting for all pending urbs to complete?
	 * only then can we forgo submitting anew
	 */
	if (waitqueue_active(&dev->wait)) {
		//if (dev->txq.qlen + dev->rxq.qlen + dev->done.qlen == 0)
		if (pdata->txq_ptp.qlen + pdata->txq_avba.qlen + pdata->txq_avbb.qlen +
		      dev->txq.qlen + dev->rxq.qlen + dev->done.qlen == 0)
		{
			wake_up_all(&dev->wait);
		}

	// or are we maybe short a few urbs?
	} else if (netif_running (dev->net) &&
		   netif_device_present (dev->net) &&
		   netif_carrier_ok(dev->net) &&
		   !timer_pending (&dev->delay) &&
		   !test_bit (EVENT_RX_HALT, &dev->flags)) {
		int	temp = dev->rxq.qlen;

		if (temp < RX_QLEN(dev)) {
			if (rx_alloc_submit(dev, GFP_ATOMIC) == -ENOLINK)
				return;
			if (temp != dev->rxq.qlen)
				netif_dbg(dev, link, dev->net,
					  "rxqlen %d --> %d\n",
					  temp, dev->rxq.qlen);
			if (dev->rxq.qlen < RX_QLEN(dev))
				tasklet_schedule (&dev->bh);
		}
		if (dev->txq.qlen < TX_QLEN (dev))
			netif_wake_subqueue (dev->net, TC9560_TX_LEG);
		if (pdata->txq_avba.qlen < TX_QLEN (dev))
			netif_wake_subqueue (dev->net, TC9560_TX_AVBA);
		if (pdata->txq_avbb.qlen < TX_QLEN (dev))
			netif_wake_subqueue (dev->net, TC9560_TX_AVBB);
		if (pdata->txq_ptp.qlen < TX_QLEN (dev))
		{
			netif_wake_subqueue (dev->net, TC9560_TX_PTP);
		}
	}
}


/*-------------------------------------------------------------------------
 *
 * USB Device Driver support
 *
 *-------------------------------------------------------------------------*/

// precondition: never called in_interrupt


static const struct net_device_ops usbnet_netdev_ops = {
	.ndo_open		= tc9560_usbnet_open,
	.ndo_stop		= tc9560_usbnet_stop,
	.ndo_start_xmit		= tc9560_usbnet_start_xmit,
	.ndo_tx_timeout		= tc9560_usbnet_tx_timeout,
	.ndo_set_rx_mode	= usbnet_set_rx_mode,
	.ndo_change_mtu		= usbnet_change_mtu,
	.ndo_set_mac_address 	= eth_mac_addr,
	.ndo_validate_addr	= eth_validate_addr,
};

/*-------------------------------------------------------------------------*/

// precondition: never called in_interrupt

static struct device_type wlan_type = {
	.name	= "wlan",
};

static struct device_type wwan_type = {
	.name	= "wwan",
};

int
tc9560_usbnet_probe (struct usb_interface *udev, const struct usb_device_id *prod)
{
	struct usbnet			*dev;
	struct tc9560_usbnet	  *pdata;
	struct net_device		*net;
	struct usb_host_interface	*interface;
	struct driver_info		*info;
	struct usb_device		*xdev;
	int				status;
	const char			*name;
	struct usb_driver 	*driver = to_usb_driver(udev->dev.driver);

	/* usbnet already took usb runtime pm, so have to enable the feature
	 * for usb interface, otherwise usb_autopm_get_interface may return
	 * failure if RUNTIME_PM is enabled.
	 */
	if (!driver->supports_autosuspend) {
		driver->supports_autosuspend = 1;
		pm_runtime_enable(&udev->dev);
	}

	name = udev->dev.driver->name;
	info = (struct driver_info *) prod->driver_info;
	if (!info) {
		dev_dbg (&udev->dev, "blacklisted by %s\n", name);
		return -ENODEV;
	}
	xdev = interface_to_usbdev (udev);
	interface = udev->cur_altsetting;

	status = -ENOMEM;

	// set up our own records
	net = alloc_etherdev_mq(sizeof(struct tc9560_usbnet), 5);
	// net = alloc_etherdev(sizeof(*dev));
	if (!net)
		goto out;


	/* netdev_printk() needs this so do it as early as possible */
	SET_NETDEV_DEV(net, &udev->dev);

	pdata = netdev_priv(net);
	dev = &(pdata->dev);
	dev->udev = xdev;
	dev->intf = udev;
	dev->driver_info = info;
	dev->driver_name = name;
	dev->msg_enable = netif_msg_init (msg_level, NETIF_MSG_DRV
				| NETIF_MSG_PROBE | NETIF_MSG_LINK);
	init_waitqueue_head(&dev->wait);
	skb_queue_head_init (&dev->rxq);
	skb_queue_head_init (&dev->txq);
	skb_queue_head_init (&pdata->txq_ptp);
	skb_queue_head_init (&pdata->txq_avba);
	skb_queue_head_init (&pdata->txq_avbb);
	skb_queue_head_init (&dev->done);
	skb_queue_head_init(&dev->rxq_pause);
	dev->bh.func = usbnet_bh;
	dev->bh.data = (unsigned long) pdata;
	INIT_WORK (&dev->kevent, kevent);
	init_usb_anchor(&dev->deferred);
	dev->delay.function = usbnet_bh;
	dev->delay.data = (unsigned long) pdata;
	init_timer (&dev->delay);
	mutex_init (&dev->phy_mutex);
	mutex_init(&dev->interrupt_mutex);
	dev->interrupt_count = 0;

	dev->net = net;
	strcpy (net->name, "usb%d");
	memcpy (net->dev_addr, node_id, sizeof node_id);

	/* rx and tx sides can use different message sizes;
	 * bind() should set rx_urb_size in that case.
	 */
	dev->hard_mtu = net->mtu + net->hard_header_len;
#if 0
// dma_supported() is deeply broken on almost all architectures
	// possible with some EHCI controllers
	if (dma_supported (&udev->dev, DMA_BIT_MASK(64)))
		net->features |= NETIF_F_HIGHDMA;
#endif

	net->netdev_ops = &usbnet_netdev_ops;
	net->watchdog_timeo = TX_TIMEOUT_JIFFIES;
	net->ethtool_ops = &usbnet_ethtool_ops;

	// allow device-specific bind/init procedures
	// NOTE net->name still not usable ...
	if (info->bind) {
		status = info->bind (dev, udev);
		if (status < 0)
			goto out1;

		// heuristic:  "usb%d" for links we know are two-host,
		// else "eth%d" when there's reasonable doubt.  userspace
		// can rename the link if it knows better.
		if ((dev->driver_info->flags & FLAG_ETHER) != 0 &&
		    ((dev->driver_info->flags & FLAG_POINTTOPOINT) == 0 ||
		     (net->dev_addr [0] & 0x02) == 0))
			strcpy (net->name, "eth%d");
		/* WLAN devices should always be named "wlan%d" */
		if ((dev->driver_info->flags & FLAG_WLAN) != 0)
			strcpy(net->name, "wlan%d");
		/* WWAN devices should always be named "wwan%d" */
		if ((dev->driver_info->flags & FLAG_WWAN) != 0)
			strcpy(net->name, "wwan%d");

		/* devices that cannot do ARP */
		if ((dev->driver_info->flags & FLAG_NOARP) != 0)
			net->flags |= IFF_NOARP;

		/* maybe the remote can't receive an Ethernet MTU */
		if (net->mtu > (dev->hard_mtu - net->hard_header_len))
			net->mtu = dev->hard_mtu - net->hard_header_len;
	} 
	else if (!info->in || !info->out)
	{		
		status = usbnet_get_endpoints (dev, udev);
	}
	else {
		dev->in = usb_rcvbulkpipe (xdev, info->in);
		dev->out = usb_sndbulkpipe (xdev, info->out);
		if (!(info->flags & FLAG_NO_SETINT))
			status = usb_set_interface (xdev,
				interface->desc.bInterfaceNumber,
				interface->desc.bAlternateSetting);
		else
			status = 0;

	}
	if (status >= 0 && dev->status)
		status = init_status (dev, udev);
	if (status < 0)
		goto out3;

	if (!dev->rx_urb_size)
		dev->rx_urb_size = dev->hard_mtu;
	dev->maxpacket = usb_maxpacket (dev->udev, dev->out, 1);

	/* let userspace know we have a random address */
	if (ether_addr_equal(net->dev_addr, node_id))
		net->addr_assign_type = NET_ADDR_RANDOM;

	if ((dev->driver_info->flags & FLAG_WLAN) != 0)
		SET_NETDEV_DEVTYPE(net, &wlan_type);
	if ((dev->driver_info->flags & FLAG_WWAN) != 0)
		SET_NETDEV_DEVTYPE(net, &wwan_type);

	/* initialize max rx_qlen and tx_qlen */
	usbnet_update_max_qlen(dev);

	if (dev->can_dma_sg && !(info->flags & FLAG_SEND_ZLP) &&
		!(info->flags & FLAG_MULTI_PACKET)) {
		dev->padding_pkt = kzalloc(1, GFP_ATOMIC);
		if (!dev->padding_pkt) {
			status = -ENOMEM;
			goto out4;
		}
	}

	status = register_netdev (net);
	if (status)
		goto out5;
	netif_info(dev, probe, dev->net,
		   "register '%s' at usb-%s-%s, %s, %pM\n",
		   udev->dev.driver->name,
		   xdev->bus->bus_name, xdev->devpath,
		   dev->driver_info->description,
		   net->dev_addr);

	// ok, it's ready to go.
	usb_set_intfdata (udev, dev);

	netif_device_attach (net);

	if (dev->driver_info->flags & FLAG_LINK_INTR)
		usbnet_link_change(dev, 0, 0);

	return 0;

out5:
	kfree(dev->padding_pkt);
out4:
	usb_free_urb(dev->interrupt);
out3:
	if (info->unbind)
		info->unbind (dev, udev);
out1:
	free_netdev(net);
out:
	return status;
}







