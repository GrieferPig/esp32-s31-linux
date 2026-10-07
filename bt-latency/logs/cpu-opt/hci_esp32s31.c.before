// SPDX-License-Identifier: GPL-2.0-only
/* ESP32-S31 Bluetooth controller HCI front end. */

#include <linux/delay.h>
#include <linux/esp32s31-radio.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/printk.h>
#include <linux/skbuff.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>

#include <net/bluetooth/bluetooth.h>
#include <net/bluetooth/hci_core.h>

struct s31_hci {
	struct hci_dev *hdev;
	struct delayed_work register_work;
	struct miscdevice direct_miscdev;
	wait_queue_head_t direct_waitq;
	struct mutex direct_read_lock;
	struct mutex direct_write_lock;
	atomic_t direct_open;
	bool direct_hci;
	bool radio_registered;
	bool suspended;
	bool reset_pending;
	u8 direct_write_buf[ESP32S31_RADIO_HCI_FRAME_MAX];
};

static struct s31_hci s31_hci;

static int s31_hci_open(struct hci_dev *hdev)
{
	int ret;

	/* Powering on hci0 enables the integrated controller via the serialized
	 * radio core. */
	ret = esp32s31_radio_bt_enable();
	if (ret && ret != -EALREADY)
		return ret;
	return 0;
}

static int s31_hci_close(struct hci_dev *hdev)
{
	/* The S31 controller is initialised for the lifetime of the unified radio
	 * module.  Its closed Classic page-scan state is not restart-safe after
	 * esp_bt_controller_disable(): the next enable asserts in olc_pscan.  Let
	 * modem sleep quiesce an idle controller instead; the measured idle path
	 * is below 0.04 CPU and avoids turning an hci0 close into a fatal restart. */
	return 0;
}

static int s31_hci_flush(struct hci_dev *hdev)
{
	return 0;
}

static int s31_hci_send_frame(struct hci_dev *hdev, struct sk_buff *skb)
{
	u8 type = hci_skb_pkt_type(skb);
	u16 opcode = 0;
	int ret;

	if (type == HCI_COMMAND_PKT && skb->len >= 3)
		opcode = skb->data[0] | (skb->data[1] << 8);
	if (opcode == HCI_OP_WRITE_CLASS_OF_DEV)
		pr_info("esp32s31-hci: TX 0x0c24 len=%u cod=%*ph\n",
			skb->len, (int)min_t(unsigned int, 3, skb->len - 3),
			skb->data + 3);

	ret = esp32s31_radio_hci_send(type, skb->data, skb->len);
	if (ret) {
		hdev->stat.err_tx++;
		return ret;
	}
	switch (type) {
	case HCI_COMMAND_PKT:
		hdev->stat.cmd_tx++;
		break;
	case HCI_ACLDATA_PKT:
		hdev->stat.acl_tx++;
		break;
	case HCI_SCODATA_PKT:
		hdev->stat.sco_tx++;
		break;
	}
	hdev->stat.byte_tx += skb->len;
	kfree_skb(skb);
	return 0;
}

static void s31_hci_receive(void *context, const u8 *frame, size_t length)
{
	struct s31_hci *radio = context;
	struct sk_buff *skb;
	u16 opcode;

	if (!radio->hdev || length < 2)
		return;
	/* The BTDM controller emits an unsolicited NOP command-complete
	 * (04 0e 03 01 00 00) before the first host command is sent.  If we
	 * forward it, the HCI core sees opcode 0 while a real command is
	 * pending and logs "unexpected event for opcode 0x0000".  It is a
	 * boot quirk, not a transport error; drop only that exact frame.
	 */
	if (length == 6 && frame[0] == 0x04 && frame[1] == 0x0e &&
	    frame[2] == 0x03 && frame[3] == 0x01 &&
	    frame[4] == 0x00 && frame[5] == 0x00) {
		pr_info_once("esp32s31-hci: dropped unsolicited initial NOP complete\n");
		return;
	}
	/* Keep one compact record of the controller capability replies.  These
	 * are the source of truth used by the HCI core to expose BR/EDR and LE
	 * through the management API, and are invaluable on this serialized
	 * transport where btmon is intentionally absent from the small rootfs. */
	if (length >= 7 && frame[0] == HCI_EVENT_PKT && frame[1] == 0x0e) {
		opcode = frame[4] | (frame[5] << 8);
		if (opcode == 0x1003 && length >= 15)
			pr_info("esp32s31-hci: local features status=%u %*ph\n",
				frame[6], 8, frame + 7);
		else if (opcode == 0x2003 && length >= 15)
			pr_info("esp32s31-hci: LE features status=%u %*ph\n",
				frame[6], 8, frame + 7);
		else if (opcode == 0x1004 && length >= 8)
			pr_info("esp32s31-hci: extended features status=%u page=%u %*ph\n",
				frame[6], frame[7],
				(int)min_t(size_t, 8, length - 9), frame + 9);
	}
	skb = bt_skb_alloc(length - 1, GFP_KERNEL);
	if (!skb) {
		radio->hdev->stat.err_rx++;
		return;
	}
	hci_skb_pkt_type(skb) = frame[0];
	skb_put_data(skb, frame + 1, length - 1);
	/* ESP-IDF's S31 controller-only VHCI transport currently reports two
	 * capabilities inconsistently in dual mode:
	 *
	 *  - Read Local Supported Features sets No BR/EDR even though the BR/EDR
	 *    stack is linked, initialized and enabled.
	 *  - Read LE Local Supported Features advertises extended advertising
	 *    and extended scanner filter policy while the matching extended
	 *    command path is not reliable over this controller build.
	 *
	 * Correct the feature replies at this compatibility boundary.  Linux then
	 * initializes the compiled-in classic controller and uses legacy LE
	 * advertising/scanning instead of repeatedly issuing unsupported extended
	 * commands.
	 */
	if (length >= 15 && frame[0] == HCI_EVENT_PKT && frame[1] == 0x0e &&
	    frame[6] == 0) {
		if (opcode == 0x1003)
			skb->data[10] &= ~LMP_NO_BREDR;
		else if (opcode == 0x2003) {
			skb->data[6] &= ~HCI_LE_EXT_SCAN_POLICY;
			skb->data[7] &= ~HCI_LE_EXT_ADV;
		}
	}
	radio->hdev->stat.byte_rx += length - 1;
	if (hci_recv_frame(radio->hdev, skb) < 0)
		radio->hdev->stat.err_rx++;
}

static const struct esp32s31_radio_hci_ops s31_hci_ops = {
	.receive = s31_hci_receive,
};

static void s31_hci_direct_rx_ready(void *context)
{
	struct s31_hci *radio = context;

	wake_up_interruptible(&radio->direct_waitq);
}

static const struct esp32s31_radio_hci_ops s31_hci_direct_ops = {
	.rx_ready = s31_hci_direct_rx_ready,
};

static int s31_hci_direct_open(struct inode *inode, struct file *file)
{
	struct s31_hci *radio = &s31_hci;
	int ret;

	mutex_lock(&radio->direct_write_lock);
	if (READ_ONCE(radio->suspended) ||
	    atomic_cmpxchg(&radio->direct_open, 0, 1)) {
		ret = -EBUSY;
		goto unlock;
	}
	ret = esp32s31_radio_hci_register(&s31_hci_direct_ops, radio);
	if (ret)
		goto clear_open;
	esp32s31_radio_hci_purge();
	ret = esp32s31_radio_bt_enable();
	if (ret && ret != -EALREADY)
		goto unregister;
	file->private_data = radio;
	radio->reset_pending = false;
	ret = nonseekable_open(inode, file);
	goto unlock;

unregister:
	esp32s31_radio_hci_unregister(&s31_hci_direct_ops, radio);
clear_open:
	atomic_set(&radio->direct_open, 0);
unlock:
	mutex_unlock(&radio->direct_write_lock);
	return ret;
}

static int s31_hci_direct_release(struct inode *inode, struct file *file)
{
	struct s31_hci *radio = file->private_data;

	/* Keep BTDM enabled and sleeping across userspace service restarts.  The
	 * IDF controller's disable/enable lifecycle leaves the Classic page-scan
	 * state inconsistent, while unregistering the host endpoint is sufficient
	 * to prevent delivery into a closing file. */
	esp32s31_radio_hci_unregister(&s31_hci_direct_ops, radio);
	esp32s31_radio_hci_purge();
	atomic_set(&radio->direct_open, 0);
	wake_up_interruptible(&radio->direct_waitq);
	return 0;
}

static ssize_t s31_hci_direct_read(struct file *file, char __user *buffer,
				   size_t count, loff_t *offset)
{
	struct s31_hci *radio = file->private_data;
	const bool batch = count > ESP32S31_RADIO_HCI_FRAME_MAX;
	const u8 *frame;
	size_t copied = 0;
	size_t length;
	unsigned int frames = 0;
	u8 prefix[2];
	int ret;

	if (count < 2)
		return -EMSGSIZE;
	if (mutex_lock_interruptible(&radio->direct_read_lock))
		return -ERESTARTSYS;
	for (;;) {
		if (READ_ONCE(radio->suspended)) {
			ret = -EAGAIN;
			break;
		}
		if (radio->reset_pending) {
			static const u8 reset_event[] = { HCI_EVENT_PKT, HCI_EV_HARDWARE_ERROR, 1, 1 };
			u8 record[6] = { sizeof(reset_event), 0 };
			size_t bytes = sizeof(reset_event) + (batch ? 2 : 0);

			if (count < bytes) {
				ret = -EMSGSIZE;
				break;
			}
			memcpy(record + (batch ? 2 : 0), reset_event, sizeof(reset_event));
			ret = copy_to_user(buffer, record, bytes) ? -EFAULT : bytes;
			if (ret > 0)
				radio->reset_pending = false;
			break;
		}
		ret = esp32s31_radio_hci_peek(&frame, &length);
		if (ret != -ENODATA)
			break;
		mutex_unlock(&radio->direct_read_lock);
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;
		ret = wait_event_interruptible(radio->direct_waitq,
				READ_ONCE(radio->suspended) || READ_ONCE(radio->reset_pending) ||
				esp32s31_radio_hci_rx_pending());
		if (ret)
			return ret;
		if (mutex_lock_interruptible(&radio->direct_read_lock))
			return -ERESTARTSYS;
	}
	while (!ret) {
		size_t record_length = length + (batch ? sizeof(prefix) : 0);
		size_t frame_offset = copied;

		if (record_length > count - copied) {
			ret = copied ? copied : -EMSGSIZE;
			break;
		}
		if (batch) {
			prefix[0] = (u8)length;
			prefix[1] = (u8)(length >> 8);
			if (copy_to_user(buffer + copied, prefix, sizeof(prefix))) {
				ret = copied ? copied : -EFAULT;
				break;
			}
			frame_offset += sizeof(prefix);
		}
		if (copy_to_user(buffer + frame_offset, frame, length)) {
			ret = copied ? copied : -EFAULT;
			break;
		}
		copied += record_length;
		esp32s31_radio_hci_consume();
		frames++;
		if (!batch || frames == 8) {
			ret = copied;
			break;
		}
		ret = esp32s31_radio_hci_peek(&frame, &length);
		if (ret == -ENODATA)
			ret = copied;
	}
	mutex_unlock(&radio->direct_read_lock);
	return ret;
}

static ssize_t s31_hci_direct_write(struct file *file,
				    const char __user *buffer, size_t count,
				    loff_t *offset)
{
	struct s31_hci *radio = file->private_data;
	int ret;

	if (count < 2 || count > sizeof(radio->direct_write_buf))
		return -EMSGSIZE;
	if (mutex_lock_interruptible(&radio->direct_write_lock))
		return -ERESTARTSYS;
	if (READ_ONCE(radio->suspended)) {
		ret = -EAGAIN;
		goto out;
	}
	if (copy_from_user(radio->direct_write_buf, buffer, count)) {
		ret = -EFAULT;
		goto out;
	}
	ret = esp32s31_radio_hci_send(radio->direct_write_buf[0],
					 radio->direct_write_buf + 1, count - 1);
	if (!ret)
		ret = count;
out:
	mutex_unlock(&radio->direct_write_lock);
	return ret;
}

static __poll_t s31_hci_direct_poll(struct file *file, poll_table *wait)
{
	struct s31_hci *radio = file->private_data;
	__poll_t mask = 0;

	poll_wait(file, &radio->direct_waitq, wait);
	if (READ_ONCE(radio->suspended))
		return 0;
	if (READ_ONCE(radio->reset_pending) || esp32s31_radio_hci_rx_pending())
		mask |= EPOLLIN | EPOLLRDNORM;
	if (esp32s31_radio_hci_tx_has_space())
		mask |= EPOLLOUT | EPOLLWRNORM;
	return mask;
}

static const struct file_operations s31_hci_direct_fops = {
	.owner = THIS_MODULE,
	.open = s31_hci_direct_open,
	.release = s31_hci_direct_release,
	.read = s31_hci_direct_read,
	.write = s31_hci_direct_write,
	.poll = s31_hci_direct_poll,
	.llseek = noop_llseek,
};

static void s31_hci_register_work(struct work_struct *work)
{
	struct s31_hci *radio = container_of(to_delayed_work(work),
					       struct s31_hci, register_work);
	struct hci_dev *hdev;
	int ret;

	ret = esp32s31_radio_hci_register(&s31_hci_ops, radio);
	if (ret == -EAGAIN) {
		schedule_delayed_work(&radio->register_work, msecs_to_jiffies(100));
		return;
	}
	if (ret) {
		pr_err("esp32s31-hci: radio registration failed: %d\n", ret);
		return;
	}
	radio->radio_registered = true;

	hdev = hci_alloc_dev();
	if (!hdev) {
		esp32s31_radio_hci_unregister(&s31_hci_ops, radio);
		radio->radio_registered = false;
		return;
	}
	radio->hdev = hdev;
	hdev->bus = HCI_VIRTUAL;
	hci_set_drvdata(hdev, radio);
	hdev->open = s31_hci_open;
	hdev->close = s31_hci_close;
	hdev->flush = s31_hci_flush;
	hdev->send = s31_hci_send_frame;
	/* The S31 BTDM command table advertises LE extended scan support, but
	 * Set Extended Scan Parameters (0x2041) returns Command Disallowed when
	 * BlueZ starts discovery.  Use Linux's controller quirk so the generic
	 * stack selects the legacy LE scan commands instead of adding a command-
	 * failure workaround in the transport.
	 */
	hci_set_quirk(hdev, HCI_QUIRK_BROKEN_EXT_SCAN);
	/* Linux's generic active-discovery defaults use a 100% scan window
	 * (11.25 ms / 11.25 ms).  The integrated Wi-Fi and BLE controllers share
	 * one 2.4 GHz radio, so cap discovery at 25% duty to leave deterministic
	 * beacon/ACK airtime for an associated station. */
	hdev->le_scan_int_discovery = 0x0060;    /* 60 ms */
	hdev->le_scan_window_discovery = 0x0018; /* 15 ms */

	ret = hci_register_dev(hdev);
	if (ret) {
		pr_err("esp32s31-hci: cannot register HCI device: %d\n", ret);
		radio->hdev = NULL;
		hci_free_dev(hdev);
		esp32s31_radio_hci_unregister(&s31_hci_ops, radio);
		radio->radio_registered = false;
		return;
	}
	pr_info("esp32s31-hci: registered hci%d over serialized VHCI\n", hdev->id);
}

int s31_radio_btdm_frontend_init(struct device *parent, bool direct_hci)
{
	int ret;

	(void)parent;
	if (esp32s31_radio_is_disabled()) {
		pr_info("esp32s31-hci: disabled with radio core\n");
		return 0;
	}
	s31_hci.direct_hci = direct_hci;
	if (direct_hci) {
		init_waitqueue_head(&s31_hci.direct_waitq);
		mutex_init(&s31_hci.direct_read_lock);
		mutex_init(&s31_hci.direct_write_lock);
		atomic_set(&s31_hci.direct_open, 0);
		s31_hci.direct_miscdev.minor = MISC_DYNAMIC_MINOR;
		s31_hci.direct_miscdev.name = "s31-hci";
		s31_hci.direct_miscdev.fops = &s31_hci_direct_fops;
		s31_hci.direct_miscdev.mode = 0600;
		pr_info("esp32s31-hci: using direct BTstack H4 transport\n");
		ret = misc_register(&s31_hci.direct_miscdev);
		if (ret)
			s31_hci.direct_hci = false;
		return ret;
	}

	INIT_DELAYED_WORK(&s31_hci.register_work, s31_hci_register_work);
	schedule_delayed_work(&s31_hci.register_work, 0);
	return 0;
}

int s31_radio_btdm_frontend_suspend(void)
{
	int ret;

	if (READ_ONCE(s31_hci.suspended))
		return 0;
	if (!s31_hci.direct_hci) {
		cancel_delayed_work_sync(&s31_hci.register_work);
		if (s31_hci.hdev) {
			ret = hci_suspend_dev(s31_hci.hdev);
			if (ret)
				return ret;
		}
		WRITE_ONCE(s31_hci.suspended, true);
		return 0;
	}
	mutex_lock(&s31_hci.direct_read_lock);
	mutex_lock(&s31_hci.direct_write_lock);
	WRITE_ONCE(s31_hci.suspended, true);
	mutex_unlock(&s31_hci.direct_write_lock);
	mutex_unlock(&s31_hci.direct_read_lock);
	wake_up_interruptible(&s31_hci.direct_waitq);
	return 0;
}

int s31_radio_btdm_frontend_resume(void)
{
	int ret = 0;

	if (s31_hci.direct_hci) {
		if (atomic_read(&s31_hci.direct_open)) {
			ret = esp32s31_radio_bt_enable();
			if (ret && ret != -EALREADY)
				return ret;
			/* BTstack's Hardware Error handler restarts its HCI state
			 * machine; stale connection handles must not survive reset. */
			WRITE_ONCE(s31_hci.reset_pending, true);
		}
		WRITE_ONCE(s31_hci.suspended, false);
		wake_up_interruptible(&s31_hci.direct_waitq);
	} else {
		WRITE_ONCE(s31_hci.suspended, false);
		if (s31_hci.hdev) {
			ret = esp32s31_radio_bt_enable();
			if (ret && ret != -EALREADY)
				return ret;
			ret = hci_resume_dev(s31_hci.hdev);
			if (!ret) {
				const u8 reset[] = { HCI_EVENT_PKT, HCI_EV_HARDWARE_ERROR, 1, 1 };

				s31_hci_receive(&s31_hci, reset, sizeof(reset));
			}
		} else {
			schedule_delayed_work(&s31_hci.register_work, 0);
		}
	}
	return ret == -EALREADY ? 0 : ret;
}

void s31_radio_btdm_frontend_exit(void)
{
	if (s31_hci.direct_hci) {
		misc_deregister(&s31_hci.direct_miscdev);
		if (atomic_read(&s31_hci.direct_open)) {
			esp32s31_radio_hci_unregister(&s31_hci_direct_ops,
						       &s31_hci);
			esp32s31_radio_hci_purge();
			atomic_set(&s31_hci.direct_open, 0);
		}
		s31_hci.direct_hci = false;
		return;
	}

	cancel_delayed_work_sync(&s31_hci.register_work);
	if (s31_hci.hdev) {
		hci_unregister_dev(s31_hci.hdev);
		hci_free_dev(s31_hci.hdev);
		s31_hci.hdev = NULL;
	}
	if (s31_hci.radio_registered) {
		esp32s31_radio_hci_unregister(&s31_hci_ops, &s31_hci);
		s31_hci.radio_registered = false;
	}
}
