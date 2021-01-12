// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020 Jozsef Horvath <info@ministro.hu>
 *
 */
#include <linux/bitops.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/gpio/driver.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/regmap.h>
#include <linux/serial_core.h>
#include <linux/serial.h>
#include <linux/tty.h>
#include <linux/tty_flip.h>
#include <linux/spi/spi.h>
#include <linux/uaccess.h>
#include <linux/string.h>
#include <linux/firmware.h>
#include <linux/timer.h>
#include <linux/debugfs.h>

#define SI4455_NAME						"Si4455"
#define SI4455_DEV_NAME						"ttySSi"
#define SI4455_UART_NRMAX					16
#define SI4455_FIFO_SIZE					64

#define SI4455_CMD_ID_EZCONFIG_CHECK				0x19
#define SI4455_CMD_ID_PART_INFO					0x01
#define SI4455_CMD_REPLY_COUNT_PART_INFO			9
#define SI4455_CMD_ID_GET_INT_STATUS				0x20
#define SI4455_CMD_REPLY_COUNT_GET_INT_STATUS			8
#define SI4455_CMD_ID_FIFO_INFO					0x15
#define SI4455_CMD_ARG_COUNT_FIFO_INFO				2
#define SI4455_CMD_REPLY_COUNT_FIFO_INFO			2
#define SI4455_CMD_FIFO_INFO_ARG_TX_BIT				0x01
#define SI4455_CMD_FIFO_INFO_ARG_RX_BIT				0x02
#define SI4455_CMD_ID_READ_CMD_BUFF				0x44
#define SI4455_CMD_ID_READ_RX_FIFO				0x77
#define SI4455_CMD_ID_WRITE_TX_FIFO				0x66
#define SI4455_CMD_ID_START_RX					0x32
#define SI4455_CMD_ARG_COUNT_START_RX				8
#define SI4455_CMD_START_RX_RXTIMEOUT_STATE_RX			8
#define SI4455_CMD_START_RX_RXVALID_STATE_SLEEP			1
#define SI4455_CMD_START_RX_RXVALID_STATE_RX			8
#define SI4455_CMD_START_RX_RXINVALID_STATE_RX			8
#define SI4455_CMD_ID_START_TX					0x31
#define SI4455_CMD_ARG_COUNT_START_TX				5
#define SI4455_CMD_ID_CHANGE_STATE				0x34
#define SI4455_CMD_ARG_COUNT_CHANGE_STATE			2
#define SI4455_CMD_CHANGE_STATE_STATE_SLEEP			1
#define SI4455_CMD_CHANGE_STATE_STATE_READY			3
#define SI4455_CMD_CHANGE_STATE_STATE_RX			8
#define SI4455_CMD_GET_CHIP_STATUS_ERROR_PEND_MASK		0x08
#define SI4455_CMD_GET_CHIP_STATUS_ERROR_PEND_BIT		0x08
#define SI4455_CMD_GET_INT_STATUS_RX_FIFO_AF_BIT		0x01
#define SI4455_CMD_GET_INT_STATUS_TX_FIFO_AE_BIT		0x02
#define SI4455_CMD_GET_INT_STATUS_PACKET_SENT_PEND_BIT		0x20
#define SI4455_CMD_GET_INT_STATUS_PACKET_RX_PEND_BIT		0x10
#define SI4455_CMD_GET_INT_STATUS_CRC_ERROR_BIT			0x08
#define SI4455_CMD_GET_INT_STATUS_CHIP_RDY_BIT			0x04
#define SI4455_CMD_GET_INT_STATUS_CMD_ERROR_BIT			0x08
#define SI4455_CMD_GET_INT_STATUS_ST_CHANGED_BIT		0x10
#define SI4455_CMD_GET_INT_STATUS_FIFO_UO_BIT			0x20
#define SI4455_CMD_ID_GET_MODEM_STATUS				0x22
#define SI4455_CMD_ARG_COUNT_GET_MODEM_STATUS			2
#define SI4455_CMD_REPLY_COUNT_GET_MODEM_STATUS			8

struct si4455_part_info {
	u8 chip_rev;
	u16 part;
	u8 pbuild;
	u16 id;
	u8 customer;
	u8 rom_id;
	u8 bond;
};

struct si4455_int_status {
	u8 int_pend;
	u8 int_status;
	u8 ph_pend;
	u8 ph_status;
	u8 modem_pend;
	u8 modem_status;
	u8 chip_pend;
	u8 chip_status;
};

struct si4455_modem_status {
	u8 modem_pend;
	u8 modem_status;
	u8 curr_rssi;
	u8 latch_rssi;
	u8 ant1_rssi;
	u8 ant2_rssi;
	u16 afc_freq_offset;
};

struct si4455_fifo_info {
	u8 rx_fifo_count;
	u8 tx_fifo_space;
};

struct si4455_port {
	struct uart_port port;
	struct dentry *dbgfs_dir;
	struct work_struct tx_work;
	struct work_struct tx_wd_work;
	struct work_struct cts_wd_work;
	struct timer_list tx_wd_timer;
	struct timer_list cts_wd_timer;
	struct mutex mutex; /* For syncing access to device */
	struct gpio_desc *shdn_gpio;
	struct si4455_part_info part_info;
	struct si4455_modem_status modem_status;
	u32 tx_channel;
	u32 rx_channel;
	u32 package_size;
	u32 current_rssi;
	u32 cts_error_count;
	u32 tx_error_count;
	int power_count;
	u32 tx_wd_timeout;
	u32 tx_pending_size;
	char ez_fw_name[255];
	bool connected;
	bool suspended;
	bool configured;
	bool cts_error;
	bool tx_pending;
	bool rx_pending;
	bool tx_stopped;
	bool rx_stopped;
};

static struct uart_driver si4455_uart = {
	.owner			= THIS_MODULE,
	.driver_name		= SI4455_NAME,
	.dev_name		= SI4455_DEV_NAME,
	.nr			= SI4455_UART_NRMAX,
};

static DEFINE_MUTEX(si4455_ports_lock);			/* race on probe */
static DECLARE_BITMAP(si4455_port_lines, SI4455_UART_NRMAX);

static int si4455_get_response(struct uart_port *port, int length, u8 *data)
{
	int ret;
	u8 data_out[] = { SI4455_CMD_ID_READ_CMD_BUFF };
	u8 *data_in;
	struct spi_transfer xfer[2];
	int timeout = 100;

	if (length > 0 && !data)
		return -EINVAL;

	data_in = kzalloc(1 + length, GFP_KERNEL);
	if (!data_in)
		return -ENOMEM;

	memset(&xfer, 0x00, sizeof(xfer));
	xfer[0].tx_buf = data_out;
	xfer[0].len = sizeof(data_out);
	xfer[1].rx_buf = data_in;
	xfer[1].len = 1 + length;

	while (--timeout > 0) {
		data_out[0] = SI4455_CMD_ID_READ_CMD_BUFF;
		ret = spi_sync_transfer(to_spi_device(port->dev), xfer,
					ARRAY_SIZE(xfer));
		if (ret) {
			dev_err(port->dev, "%s: spi_sync_transfer error (%i)\n", __func__, ret);
			break;
		}

		if (data_in[0] == 0xFF) {
			if (length > 0 && data)
				memcpy(data, &data_in[1], length);

			break;
		}
		usleep_range(100, 200);
	}
	if (!ret && timeout == 0) {
		dev_err(port->dev, "%s: timeout\n", __func__);
		ret = -EIO;
	}
	kfree(data_in);
	return ret;
}

static int si4455_poll_cts(struct uart_port *port)
{
	int ret;
	struct si4455_port *s = dev_get_drvdata(port->dev);

	ret = si4455_get_response(port, 0, NULL);
	if (ret == -EIO) {
		s->cts_error_count++;
		s->cts_error = true;
	}
	return ret;
}

static int si4455_send_command(struct uart_port *port, int length, u8 *data)
{
	int ret;

	ret = si4455_poll_cts(port);
	if (ret) {
		dev_err(port->dev,
			"%s: si4455_poll_cts error (%i)\n", __func__, ret);
		return ret;
	}

	ret = spi_write(to_spi_device(port->dev), data, length);
	if (ret) {
		dev_err(port->dev,
			"%s: spi_write error (%i)\n", __func__, ret);
	}

	return ret;
}

static int si4455_send_command_get_response(struct uart_port *port,
					    int out_length, u8 *data_out,
					    int in_length, u8 *data_in)
{
	int ret;

	ret = si4455_send_command(port, out_length, data_out);
	if (ret) {
		dev_err(port->dev,
			"%s: si4455_send_command error (%i)\n", __func__, ret);
		return ret;
	}

	return si4455_get_response(port, in_length, data_in);
}

static int si4455_read_data(struct uart_port *port, u8 command, bool poll,
			    int length, u8 *data)
{
	int ret = 0;
	u8 data_out[] = { command };
	struct spi_transfer xfer[] = {
		{
			.tx_buf = data_out,
			.len = sizeof(data_out),
		}, {
			.rx_buf = data,
			.len = length,
		}
	};

	if (poll) {
		ret = si4455_poll_cts(port);
		if (ret)
			return ret;
	}

	ret = spi_sync_transfer(to_spi_device(port->dev),
				xfer,
				ARRAY_SIZE(xfer));
	if (ret) {
		dev_err(port->dev,
			"%s: spi_sync_transfer error (%i)\n", __func__, ret);
	}

	return ret;
}

static int si4455_write_data(struct uart_port *port, u8 command, bool poll,
			     int length, const u8 *data)
{
	int ret = 0;
	u8 *data_out;

	if (poll) {
		ret = si4455_poll_cts(port);
		if (ret)
			return ret;
	}

	data_out = kzalloc(1 + length, GFP_KERNEL);
	if (!data_out)
		return -ENOMEM;

	data_out[0] = command;
	memcpy(&data_out[1], data, length);
	ret = spi_write(to_spi_device(port->dev), data_out, 1 + length);
	if (ret) {
		dev_err(port->dev,
			"%s: spi_write error (%i)\n", __func__, ret);
	}

	kfree(data_out);

	return ret;
}

static void si4455_set_power(struct si4455_port *priv, bool on)
{
	if (!priv->shdn_gpio)
		return;

	gpiod_direction_output(priv->shdn_gpio, 0);
	if (on) {
		usleep_range(14000, 15000);
		gpiod_set_value(priv->shdn_gpio, 1);
		usleep_range(14000, 15000);
	}
}

static int si4455_s_power(struct device *dev, bool on)
{
	struct si4455_port *s = dev_get_drvdata(dev);

	if (s->power_count == !on)
		si4455_set_power(s, on);
	s->power_count += on ? 1 : -1;
	WARN_ON(s->power_count < 0);

	return 0;
}

static int si4455_get_part_info(struct uart_port *port,
				struct si4455_part_info *result)
{
	int ret;
	u8 data_out[] = { SI4455_CMD_ID_PART_INFO };
	u8 data_in[SI4455_CMD_REPLY_COUNT_PART_INFO];

	ret = si4455_send_command_get_response(port, sizeof(data_out), data_out,
					       sizeof(data_in), data_in);
	if (ret) {
		dev_err(port->dev,
			"%s: si4455_send_command_get_response error (%i)\n",
			__func__, ret);
		return ret;
	}

	result->chip_rev = data_in[0];
	memcpy(&result->part, &data_in[1], sizeof(result->part));
	result->pbuild = data_in[3];
	memcpy(&result->id, &data_in[4], sizeof(result->id));
	result->customer = data_in[6];
	result->rom_id = data_in[7];
	result->bond = data_in[8];

	return 0;
}

static int si4455_get_int_status(struct uart_port *port, u8 ph_clear,
				 u8 modem_clear, u8 chip_clear,
				 struct si4455_int_status *result)
{
	int ret;
	u8 data_out[] = {
		SI4455_CMD_ID_GET_INT_STATUS,
		ph_clear,
		modem_clear,
		chip_clear
	};
	u8 data_in[SI4455_CMD_REPLY_COUNT_GET_INT_STATUS];

	ret = si4455_send_command_get_response(port, sizeof(data_out), data_out,
					       sizeof(data_in), data_in);
	if (ret) {
		dev_err(port->dev,
			"%s: si4455_send_command_get_response error (%i)\n",
			__func__, ret);
		return ret;
	}

	result->int_pend       = data_in[0];
	result->int_status     = data_in[1];
	result->ph_pend        = data_in[2];
	result->ph_status      = data_in[3];
	result->modem_pend     = data_in[4];
	result->modem_status   = data_in[5];
	result->chip_pend      = data_in[6];
	result->chip_status    = data_in[7];

	return 0;
}

static int si4455_get_modem_status(struct uart_port *port, u8 modem_clear,
				   struct si4455_modem_status *result)
{
	int ret;
	u8 data_out[] = {
		SI4455_CMD_ID_GET_MODEM_STATUS,
		modem_clear,
	};
	u8 data_in[SI4455_CMD_REPLY_COUNT_GET_MODEM_STATUS];

	ret = si4455_send_command_get_response(port, sizeof(data_out), data_out,
					       sizeof(data_in), data_in);
	if (ret) {
		dev_err(port->dev,
			"%s: si4455_send_command_get_response error (%i)\n",
			__func__, ret);
		return ret;
	}

	result->modem_pend      = data_in[0];
	result->modem_status    = data_in[1];
	result->curr_rssi       = data_in[2];
	result->latch_rssi      = data_in[3];
	result->ant1_rssi       = data_in[4];
	result->ant2_rssi       = data_in[5];
	memcpy(&result->afc_freq_offset, &data_in[6],
	       sizeof(result->afc_freq_offset));

	return 0;
}

static int si4455_fifo_info(struct uart_port *port, u8 fifo,
			    struct si4455_fifo_info *result)
{
	int ret;
	u8 data_out[SI4455_CMD_ARG_COUNT_FIFO_INFO] = {
		SI4455_CMD_ID_FIFO_INFO, fifo
	};
	u8 data_in[SI4455_CMD_REPLY_COUNT_FIFO_INFO] = { 0 };

	ret = si4455_send_command_get_response(port, sizeof(data_out), data_out,
					       sizeof(data_in), data_in);
	if (ret) {
		dev_err(port->dev,
			"%s: si4455_send_command_get_response error (%i)\n",
			__func__, ret);
		return ret;
	}

	result->rx_fifo_count  = data_in[0];
	result->tx_fifo_space  = data_in[1];

	return 0;
}

static int si4455_read_rx_fifo(struct uart_port *port, int length, u8 *data)
{
	return si4455_read_data(port, SI4455_CMD_ID_READ_RX_FIFO, false,
				length, data);
}

static int si4455_write_tx_fifo(struct uart_port *port, int length, u8 *data)
{
	return si4455_write_data(port, SI4455_CMD_ID_WRITE_TX_FIFO, false,
				 length, data);
}

static int si4455_rx(struct uart_port *port, u32 channel, u8 condition,
		     u16 length, u8 next_state1, u8 next_state2,
		     u8 next_state3)
{
	u8 data_out[SI4455_CMD_ARG_COUNT_START_RX];

	data_out[0] = SI4455_CMD_ID_START_RX;
	data_out[1] = channel;
	data_out[2] = condition;
	data_out[3] = (u8)(length >> 8);
	data_out[4] = (u8)(length);
	data_out[5] = next_state1;
	data_out[6] = next_state2;
	data_out[7] = next_state3;

	return si4455_send_command(port, SI4455_CMD_ARG_COUNT_START_RX,
				   data_out);
}

static int si4455_tx(struct uart_port *port, u8 channel, u8 condition,
		     u16 length)
{
	struct si4455_port *s = dev_get_drvdata(port->dev);
	u8 data_out[SI4455_CMD_ARG_COUNT_START_TX + 1];
	u8 out_length = SI4455_CMD_ARG_COUNT_START_TX;

	if (s->part_info.rom_id == 6)
		out_length += 1;

	data_out[0] = SI4455_CMD_ID_START_TX;
	data_out[1] = channel;
	data_out[2] = condition;
	data_out[3] = (u8)(length >> 8);
	data_out[4] = (u8)(length);
	if (s->part_info.rom_id == 6)
		data_out[5] = 0x44;

	return si4455_send_command(port, out_length, data_out);
}

static int si4455_change_state(struct uart_port *port, u8 next_state1)
{
	u8 data_out[SI4455_CMD_ARG_COUNT_CHANGE_STATE];

	data_out[0] = SI4455_CMD_ID_CHANGE_STATE;
	data_out[1] = (u8)next_state1;

	return si4455_send_command(port, SI4455_CMD_ARG_COUNT_CHANGE_STATE,
				   data_out);
}

static int si4455_begin_tx(struct uart_port *port, u32 channel, int length,
			   u8 *data)
{
	int ret = 0;
	struct si4455_int_status int_status = { 0 };
	struct si4455_fifo_info fifo_info = { 0 };

	if (length > SI4455_FIFO_SIZE || length < 0)
		return -EINVAL;

	ret = si4455_get_int_status(port, 0, 0, 0, &int_status);
	if (ret) {
		dev_err(port->dev, "%s: si4455_get_int_status error (%i)\n",
			__func__, ret);
		return ret;
	}

	ret = si4455_fifo_info(port, SI4455_CMD_FIFO_INFO_ARG_TX_BIT,
			       &fifo_info);
	if (ret) {
		dev_err(port->dev, "%s: si4455_fifo_info error (%i)\n",
			__func__, ret);
		return ret;
	}

	ret = si4455_write_tx_fifo(port, (u16)length, data);
	if (ret) {
		dev_err(port->dev, "%s: si4455_write_tx_fifo error (%i)\n",
			__func__, ret);
		return ret;
	}

	ret = si4455_tx(port, channel, 0x10, length);
	if (ret) {
		dev_err(port->dev, "%s: si4455_tx error (%i)\n",
			__func__, ret);
		return ret;
	}

	return 0;
}

static int si4455_end_tx(struct uart_port *port)
{
	int ret = 0;
	struct si4455_int_status int_status = { 0 };

	ret = si4455_get_int_status(port, 0, 0, 0, &int_status);
	if (ret) {
		dev_err(port->dev, "%s: si4455_get_int_status error (%i)\n",
			__func__, ret);
		return ret;
	}

	return 0;
}

static int si4455_begin_rx(struct uart_port *port, u32 channel, u32 length)
{
	struct si4455_port *s = dev_get_drvdata(port->dev);
	int ret = 0;
	struct si4455_int_status int_status = { 0 };
	struct si4455_fifo_info fifo_info = { 0 };

	ret = si4455_get_int_status(port, 0, 0, 0, &int_status);
	if (ret) {
		dev_err(port->dev, "%s: si4455_get_int_status error (%i)\n",
			__func__, ret);
		return ret;
	}

	ret = si4455_fifo_info(port, SI4455_CMD_FIFO_INFO_ARG_RX_BIT,
			       &fifo_info);
	if (ret) {
		dev_err(port->dev, "%s: si4455_fifo_info error (%i)\n",
			__func__, ret);
		return ret;
	}

	if (s->rx_stopped)
		return 0;

	ret = si4455_rx(port, channel, 0x00, length,
			SI4455_CMD_START_RX_RXTIMEOUT_STATE_RX,
			SI4455_CMD_START_RX_RXVALID_STATE_RX,
			SI4455_CMD_START_RX_RXINVALID_STATE_RX);
	if (ret) {
		dev_err(port->dev, "%s: si4455_rx error (%i)\n",
			__func__, ret);
		return ret;
	}

	return 0;
}

static int si4455_end_rx(struct uart_port *port, u32 length, u8 *data)
{
	return si4455_read_rx_fifo(port, length, data);
}

static int si4455_configure(struct uart_port *port, const u8 *configuration_data)
{
	int ret = 0;
	u8 col;
	u8 response;
	u8 count;
	u8 cmd;
	struct si4455_int_status int_status = { 0 };
	u8 radio_cmd[16];

	/*
	 * While cycle as far as the pointer points to a command
	 */
	while (*configuration_data != 0x00) {
		/*
		 * Commands structure in the array:
		 * --------------------------------
		 * LEN | <LEN length of data>
		 */
		count = *configuration_data++;
		cmd = *configuration_data;
		dev_dbg(port->dev, "%s: count (%u), cmd (%u)\n",
			__func__, count, cmd);

		if (count > 16 && count <= 128 && cmd == SI4455_CMD_ID_WRITE_TX_FIFO) {
			/*
			 * Load array to the device
			 */
			configuration_data++;
			ret = si4455_write_data(port, SI4455_CMD_ID_WRITE_TX_FIFO,
						true, count - 1, configuration_data);
			if (ret) {
				dev_err(port->dev, "%s: si4455_write_data error (%i)\n",
					__func__, ret);
				break;
			}

			/*
			 * Point to the next command
			 */
			configuration_data += count - 1;

			/*
			 * Continue command interpreter
			 */
			continue;
		} else if (count > 16) {
			/*
			 * Number of command bytes exceeds
			 * maximal allowable length
			 */
			ret = -EINVAL;
			break;
		}

		for (col = 0u; col < count; col++) {
			radio_cmd[col] = *configuration_data;
			configuration_data++;
		}

		dev_dbg(port->dev, "%s: radio_cmd[0] (%u)\n", __func__, radio_cmd[0]);
		ret = si4455_send_command_get_response(port, count, radio_cmd,
						       1, &response);
		if (ret) {
			dev_err(port->dev,
				"%s: si4455_send_command_get_response error (%i)\n",
				__func__, ret);
			break;
		}

		/*
		 * Check response byte of EZCONFIG_CHECK command
		 */
		if (radio_cmd[0] == SI4455_CMD_ID_EZCONFIG_CHECK) {
			if (response) {
				/*
				 * Number of command bytes exceeds
				 * maximal allowable length
				 */
				ret = -EIO;
				dev_err(port->dev, "%s: EZConfig check error (%i)\n",
					__func__, radio_cmd[0]);
				break;
			}
		}

		/*
		 * Get and clear all interrupts.  An error has occurred...
		 */
		si4455_get_int_status(port, 0, 0, 0, &int_status);
		if (int_status.chip_pend
		    & SI4455_CMD_GET_CHIP_STATUS_ERROR_PEND_MASK) {
			ret = -EIO;
			dev_err(port->dev, "%s: chip error (%i)\n",
				__func__, int_status.chip_pend);
			break;
		}
	}

	return ret;
}

static int si4455_re_configure(struct uart_port *port, const struct firmware *configuration)
{
	int ret = 0;
	struct si4455_port *s = dev_get_drvdata(port->dev);

	s->configured = 0;
	if (s->power_count == 0)
		si4455_s_power(port->dev, true);

	ret = si4455_configure(port, configuration->data);
	if (ret == 0) {
		s->configured = true;
		s->cts_error = false;
	}
	return ret;
}

static int si4455_start_tx_xmit(struct uart_port *port)
{
	int ret;
	struct si4455_port *s = dev_get_drvdata(port->dev);
	struct circ_buf *xmit = &port->state->xmit;
	u32 tx_pending;
	u32 tx_to_end;
	u8 *data;
	u8 *payload;
	u32 max_length;

	if (s->tx_stopped)
		return 0;

	tx_pending = uart_circ_chars_pending(xmit);
	if (tx_pending == 0 || tx_pending < s->package_size)
		return 0;

	max_length = (s->package_size == 0) ? SI4455_FIFO_SIZE - 3 : s->package_size;
	tx_pending = (tx_pending > max_length) ? max_length : tx_pending;
	if (s->package_size == 0)
		data = kzalloc(tx_pending + 1, GFP_KERNEL);
	else
		data = kzalloc(tx_pending, GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	if (s->package_size == 0) {
		data[0] = tx_pending;
		payload = &data[1];
	} else {
		payload = data;
	}

	tx_to_end = CIRC_CNT_TO_END(xmit->head, xmit->tail, UART_XMIT_SIZE);
	if (tx_to_end < tx_pending) {
		memcpy(payload, xmit->buf + xmit->tail, tx_to_end);
		memcpy(&payload[tx_to_end], xmit->buf, tx_pending - tx_to_end);
	} else {
		memcpy(payload, xmit->buf + xmit->tail, tx_pending);
	}

	if (s->package_size == 0)
		ret = si4455_begin_tx(port, s->tx_channel, tx_pending + 1, data);
	else
		ret = si4455_begin_tx(port, s->tx_channel, tx_pending, data);
	if (!ret) {
		s->tx_pending = true;
		s->tx_pending_size = tx_pending;
		uart_handle_cts_change(&s->port, 0);
		mod_timer(&s->tx_wd_timer, jiffies + msecs_to_jiffies(s->tx_wd_timeout));
	}

	kfree(data);

	return ret;
}

static int si4455_cancel_tx(struct uart_port *port)
{
	int ret = 0;
	struct si4455_port *s = dev_get_drvdata(port->dev);

	if (s->tx_pending) {
		si4455_end_tx(port);
		s->tx_pending = false;
		s->tx_pending_size = 0;
		uart_handle_cts_change(&s->port, TIOCM_CTS);
		ret = si4455_change_state(port, SI4455_CMD_CHANGE_STATE_STATE_SLEEP);
	}
	return ret;
}

static int si4455_do_work(struct uart_port *port)
{
	int ret = 0;
	struct si4455_port *s = dev_get_drvdata(port->dev);
	struct circ_buf *xmit = &port->state->xmit;

	mutex_lock(&s->mutex);
	if (!s->suspended && s->connected && s->configured && s->power_count > 0) {
		if (!(uart_circ_empty(xmit) || uart_tx_stopped(port) || s->tx_pending))
			ret = si4455_start_tx_xmit(port);

		if (!ret && !s->tx_pending)
			ret = si4455_begin_rx(port, s->rx_channel, s->package_size);
	}
	mutex_unlock(&s->mutex);
	return ret;
}

static void si4455_handle_rx_pend(struct si4455_port *s, struct si4455_fifo_info *fifo_info)
{
	struct uart_port *port = &s->port;
	u8 *data;
	int sret = 0;
	int i = 0;
	u32 length;

	length = (s->package_size == 0) ? fifo_info->rx_fifo_count : s->package_size;

	data = kzalloc(length, GFP_KERNEL);
	if (!data)
		return;

	sret = si4455_end_rx(port, length, data);
	if (sret) {
		dev_err(port->dev, "%s: si4455_end_rx error (%i)\n",
			__func__, sret);
	} else {
		if (!s->rx_stopped) {
			for (i = 0; i < length; i++) {
				uart_insert_char(port, 0, 0, data[i], TTY_NORMAL);
				port->icount.rx++;
			}
			tty_flip_buffer_push(&port->state->port);
		}
	}
	kfree(data);
}

static void si4455_handle_tx_pend(struct si4455_port *s)
{
	struct uart_port *port = &s->port;
	struct circ_buf *xmit = &port->state->xmit;
	u32 sent;

	if (s->tx_pending) {
		sent = (s->package_size == 0) ? s->tx_pending_size
			: s->package_size;
		port->icount.tx += sent;
		xmit->tail = (xmit->tail + sent) & (UART_XMIT_SIZE - 1);
		si4455_end_tx(port);
		s->tx_pending = 0;
		s->tx_pending_size = 0;
		uart_handle_cts_change(&s->port, TIOCM_CTS);
	}
}

static irqreturn_t si4455_ist(int irq, void *dev_id)
{
	struct si4455_port *s = (struct si4455_port *)dev_id;
	struct uart_port *port = &s->port;
	int ret;
	struct si4455_int_status int_status = { 0 };
	struct si4455_fifo_info fifo_info = { 0 };
	bool have_to_do = false;

	if (s->suspended || !s->connected || !s->configured || s->power_count == 0)
		return IRQ_NONE;

	mutex_lock(&s->mutex);
	ret = si4455_get_int_status(port, 0, 0, 0, &int_status);
	if (ret) {
		mutex_unlock(&s->mutex);
		return IRQ_NONE;
	}

	dev_dbg(port->dev, "%s: int_pend: 0x%x\n", __func__, int_status.int_pend);
	dev_dbg(port->dev, "%s: int_status: 0x%x\n", __func__, int_status.int_status);
	dev_dbg(port->dev, "%s: ph_pend: 0x%x\n", __func__, int_status.ph_pend);
	dev_dbg(port->dev, "%s: ph_status: 0x%x\n", __func__, int_status.ph_status);
	dev_dbg(port->dev, "%s: modem_pend: 0x%x\n", __func__, int_status.modem_pend);
	dev_dbg(port->dev, "%s: modem_status: 0x%x\n", __func__, int_status.modem_status);
	dev_dbg(port->dev, "%s: chip_pend: 0x%x\n", __func__, int_status.chip_pend);
	dev_dbg(port->dev, "%s: chip_status: 0x%x\n", __func__, int_status.chip_status);

	if (int_status.chip_pend & SI4455_CMD_GET_CHIP_STATUS_ERROR_PEND_BIT) {
		dev_err(port->dev, "%s: chip_pend:CMD_ERROR_PEND\n", __func__);
		si4455_change_state(port, SI4455_CMD_CHANGE_STATE_STATE_SLEEP);
		si4455_fifo_info(&s->port, SI4455_CMD_FIFO_INFO_ARG_RX_BIT,
				 &fifo_info);
		have_to_do = true;
	} else if (int_status.ph_pend & SI4455_CMD_GET_INT_STATUS_PACKET_SENT_PEND_BIT) {
		dev_dbg(port->dev, "%s: ph_pend:PACKET_SENT_PEND\n", __func__);
		si4455_change_state(port, SI4455_CMD_CHANGE_STATE_STATE_SLEEP);
		si4455_handle_tx_pend(s);
		have_to_do = true;
	} else if (int_status.ph_pend & SI4455_CMD_GET_INT_STATUS_PACKET_RX_PEND_BIT) {
		dev_dbg(port->dev, "%s: ph_pend:PACKET_RX_PEND\n", __func__);
		si4455_get_modem_status(port, 0, &s->modem_status);
		s->current_rssi = s->modem_status.curr_rssi;
		si4455_change_state(port, SI4455_CMD_CHANGE_STATE_STATE_SLEEP);
		si4455_fifo_info(port, 0, &fifo_info);
		si4455_handle_rx_pend(s, &fifo_info);
		have_to_do = true;
	} else if (int_status.ph_pend & SI4455_CMD_GET_INT_STATUS_CRC_ERROR_BIT) {
		dev_dbg(port->dev, "%s: ph_pend:CRC_ERROR_PEND\n", __func__);
		si4455_change_state(port, SI4455_CMD_CHANGE_STATE_STATE_SLEEP);
		si4455_fifo_info(&s->port, SI4455_CMD_FIFO_INFO_ARG_RX_BIT,
				 &fifo_info);
		have_to_do = true;
	}
	mutex_unlock(&s->mutex);
	if (have_to_do)
		si4455_do_work(port);
	return IRQ_HANDLED;
}

static void si4455_tx_wd_event(struct timer_list *t)
{
	struct si4455_port *s = from_timer(s, t, tx_wd_timer);

	if (s->tx_pending)
		schedule_work(&s->tx_wd_work);
}

static void si4455_tx_wd_proc(struct work_struct *ws)
{
	struct si4455_port *s = container_of(ws, struct si4455_port, tx_wd_work);
	bool have_to_work = false;

	mutex_lock(&s->mutex);
	if (s->connected && s->tx_pending) {
		si4455_cancel_tx(&s->port);
		s->tx_error_count++;
		have_to_work = true;
		dev_err(s->port.dev,
			"%s: curent transmit operation interrupted by wd timeout\n",
			__func__);
	}
	mutex_unlock(&s->mutex);

	if (have_to_work)
		si4455_do_work(&s->port);
}

static void si4455_cts_wd_event(struct timer_list *t)
{
	struct si4455_port *s = from_timer(s, t, cts_wd_timer);

	if (s->cts_error)
		schedule_work(&s->cts_wd_work);
	else
		mod_timer(&s->cts_wd_timer, jiffies + msecs_to_jiffies(100));
}

static void si4455_cts_wd_proc(struct work_struct *ws)
{
	struct si4455_port *s = container_of(ws, struct si4455_port, cts_wd_work);
	const struct firmware *ez_fw = NULL;
	bool have_to_work = false;
	int ret;

	mutex_lock(&s->mutex);
	if (s->cts_error) {
		dev_err(s->port.dev, "%s: interface recovery\n", __func__);
		ret = request_firmware(&ez_fw, s->ez_fw_name, s->port.dev);
		if (ret) {
			dev_err(s->port.dev, "%s: firmware(%s) request error (%i)\n",
				__func__, s->ez_fw_name, ret);
		} else {
			si4455_s_power(s->port.dev, false);
			ret = si4455_re_configure(&s->port, ez_fw);
			release_firmware(ez_fw);
			if (ret) {
				dev_err(s->port.dev, "%s: device configuration error (%i)\n",
					__func__, ret);
			}
		}
		have_to_work = !ret;
	}
	if (s->connected)
		mod_timer(&s->cts_wd_timer, jiffies + msecs_to_jiffies(100));
	mutex_unlock(&s->mutex);

	if (have_to_work)
		si4455_do_work(&s->port);
}

static void si4455_tx_proc(struct work_struct *ws)
{
	struct si4455_port *s = container_of(ws, struct si4455_port, tx_work);

	si4455_do_work(&s->port);
}

static unsigned int si4455_tx_empty(struct uart_port *port)
{
	struct si4455_port *s = dev_get_drvdata(port->dev);

	return s->tx_pending ? 0 : TIOCSER_TEMT;
}

static unsigned int si4455_get_mctrl(struct uart_port *port)
{
	int ret;
	struct si4455_port *s = dev_get_drvdata(port->dev);

	/*
	 * there is no a continuous real "carrier" like on phone line,
	 * but after configuration, the interface is ready to use the physical
	 * transport channel
	 */
	ret = s->configured ? TIOCM_CAR | TIOCM_DSR : 0;
	ret |= s->tx_pending ? 0 : TIOCM_CTS;

	return ret;
}

static void si4455_set_mctrl(struct uart_port *port, unsigned int mctrl)
{
}

static void si4455_set_termios(struct uart_port *port, struct ktermios *termios,
			       struct ktermios *old)
{
	dev_dbg(port->dev, "termios->c_iflag = 0x%x", termios->c_iflag);
	dev_dbg(port->dev, "termios->c_oflag = 0x%x", termios->c_oflag);
	dev_dbg(port->dev, "termios->c_cflag = 0x%x", termios->c_cflag);
	dev_dbg(port->dev, "termios->c_lflag = 0x%x", termios->c_lflag);
	dev_dbg(port->dev, "termios->c_ispeed = %u", termios->c_ispeed);
	dev_dbg(port->dev, "termios->c_ospeed = %u", termios->c_ospeed);
	if ((termios->c_cflag & CSIZE) != CS8)
		dev_err(port->dev, "%s: CSIZE must be CS8\n", __func__);
}

static int si4455_startup(struct uart_port *port)
{
	struct si4455_port *s = dev_get_drvdata(port->dev);

	mutex_lock(&s->mutex);
	s->tx_pending = false;
	s->tx_stopped = false;
	s->rx_stopped = false;
	s->connected = true;
	mod_timer(&s->cts_wd_timer, jiffies + msecs_to_jiffies(100));
	mutex_unlock(&s->mutex);
	return si4455_do_work(port);
}

static void si4455_shutdown(struct uart_port *port)
{
	struct si4455_port *s = dev_get_drvdata(port->dev);

	mutex_lock(&s->mutex);
	del_timer_sync(&s->tx_wd_timer);
	del_timer_sync(&s->cts_wd_timer);
	s->connected = false;
	si4455_change_state(&s->port, SI4455_CMD_CHANGE_STATE_STATE_SLEEP);
	mutex_unlock(&s->mutex);
}

static const char *si4455_type(struct uart_port *port)
{
	struct si4455_port *s = dev_get_drvdata(port->dev);

	if (port->type != PORT_SI4455)
		return NULL;
	if (s->part_info.rom_id == 3)
		return "SI4455-B1A";
	else if (s->part_info.rom_id == 6)
		return "SI4455-C2A";

	return "SI4455(UNKNOWN-REV)";
}

static void si4455_config_port(struct uart_port *port, int flags)
{
	if (flags & UART_CONFIG_TYPE)
		port->type = PORT_SI4455;
}

static int si4455_verify_port(struct uart_port *port, struct serial_struct *s)
{
	if (s->type != PORT_UNKNOWN && s->type != PORT_SI4455)
		return -EINVAL;

	if (s->irq != port->irq)
		return -EINVAL;

	return 0;
}

static void si4455_start_tx(struct uart_port *port)
{
	struct si4455_port *s = container_of(port, struct si4455_port, port);

	s->tx_stopped = false;
	schedule_work(&s->tx_work);
}

static void si4455_stop_tx(struct uart_port *port)
{
	struct si4455_port *s = container_of(port, struct si4455_port, port);

	s->tx_stopped = true;
}

static void si4455_stop_rx(struct uart_port *port)
{
	struct si4455_port *s = container_of(port, struct si4455_port, port);

	mutex_lock(&s->mutex);
	s->rx_stopped = true;
	si4455_change_state(&s->port, SI4455_CMD_CHANGE_STATE_STATE_SLEEP);
	mutex_unlock(&s->mutex);
}

static const struct uart_ops si4455_ops = {
	.tx_empty		= si4455_tx_empty,
	/*
	 * set_mctrl: required by serial_core, but not used
	 */
	.set_mctrl		= si4455_set_mctrl,
	.get_mctrl		= si4455_get_mctrl,
	.stop_tx		= si4455_stop_tx,
	.start_tx		= si4455_start_tx,
	.stop_rx		= si4455_stop_rx,
	.startup		= si4455_startup,
	.shutdown		= si4455_shutdown,
	.set_termios		= si4455_set_termios,
	.type			= si4455_type,
	.config_port		= si4455_config_port,
	.verify_port		= si4455_verify_port,
};

static void si4455_debugfs_init(struct device *dev)
{
	struct si4455_port *s = dev_get_drvdata(dev);
	struct dentry *dbgfs_si_dir;
	struct dentry *dbgfs_partinfo_dir;

	s->dbgfs_dir = debugfs_create_dir(dev_name(dev), NULL);

	dbgfs_si_dir = debugfs_create_dir("si4455", s->dbgfs_dir);

	debugfs_create_u32("cts_error_count", 0444, dbgfs_si_dir,
			   &s->cts_error_count);

	debugfs_create_u32("tx_error_count", 0444, dbgfs_si_dir,
			   &s->tx_error_count);

	dbgfs_partinfo_dir = debugfs_create_dir("partinfo", dbgfs_si_dir);

	debugfs_create_u8("chip_rev", 0444, dbgfs_partinfo_dir,
			  &s->part_info.chip_rev);

	debugfs_create_u8("rom_id", 0444, dbgfs_partinfo_dir,
			  &s->part_info.rom_id);

	debugfs_create_u16("part", 0444, dbgfs_partinfo_dir,
			   &s->part_info.part);
}

static void si4455_debugfs_clear(struct device *dev)
{
	struct si4455_port *s = dev_get_drvdata(dev);

	debugfs_remove_recursive(s->dbgfs_dir);
}

static int __maybe_unused si4455_suspend(struct device *dev)
{
	int ret;
	struct si4455_port *s = dev_get_drvdata(dev);

	mutex_lock(&s->mutex);
	ret = si4455_cancel_tx(&s->port);
	if (ret) {
		mutex_unlock(&s->mutex);
		dev_err(dev, "%s: si4455_cancel_tx error (%i)\n",
			__func__, ret);
		return ret;
	}

	ret = si4455_change_state(&s->port, SI4455_CMD_CHANGE_STATE_STATE_SLEEP);
	s->suspended = !ret;
	mutex_unlock(&s->mutex);

	if (ret) {
		dev_err(dev, "%s: si4455_change_state error (%i)\n",
			__func__, ret);
		return ret;
	}

	return uart_suspend_port(&si4455_uart, &s->port);
}

static int __maybe_unused si4455_resume(struct device *dev)
{
	int ret;
	struct si4455_port *s = dev_get_drvdata(dev);

	ret = uart_resume_port(&si4455_uart, &s->port);
	if (ret) {
		dev_err(dev, "%s: uart_resume_port error (%i)\n",
			__func__, ret);
		return ret;
	}

	s->suspended = false;
	s->rx_stopped = false;

	return si4455_do_work(&s->port);
}

static SIMPLE_DEV_PM_OPS(si4455_pm_ops, si4455_suspend, si4455_resume);

static ssize_t package_size_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct si4455_port *s = dev_get_drvdata(dev);

	return sprintf(buf, "%u\n", s->package_size);
}

static ssize_t package_size_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct si4455_port *s = dev_get_drvdata(dev);
	unsigned long val;
	int ret;

	ret = kstrtoul(buf, 10, &val);
	if (ret)
		return ret;

	if (val > SI4455_FIFO_SIZE)
		return -EINVAL;

	s->package_size = val;
	ret = si4455_do_work(&s->port);

	return ret ? ret : count;
}

/*
 * package_size: rw sysfs entry.
 * Sets or returns the package size.
 * The new value applied immediately.
 * Variable package size (package_size == 0)
 * currently not supported by driver.
 */
static DEVICE_ATTR_RW(package_size);

static ssize_t rx_channel_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct si4455_port *s = dev_get_drvdata(dev);

	return sprintf(buf, "%u\n", s->rx_channel);
}

static ssize_t rx_channel_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct si4455_port *s = dev_get_drvdata(dev);
	unsigned long val;
	int ret;

	ret = kstrtoul(buf, 10, &val);
	if (ret)
		return ret;

	s->rx_channel = val;
	ret = si4455_do_work(&s->port);

	return ret ? ret : count;
}

/*
 * rx_channel: rw sysfs entry.
 * Sets or returns the receive channel index.
 * The new value applied immediately.
 */
static DEVICE_ATTR_RW(rx_channel);

static ssize_t tx_channel_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct si4455_port *s = dev_get_drvdata(dev);

	return sprintf(buf, "%u\n", s->tx_channel);
}

static ssize_t tx_channel_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct si4455_port *s = dev_get_drvdata(dev);
	unsigned long val;
	int ret;

	ret = kstrtoul(buf, 10, &val);
	if (ret)
		return ret;

	s->tx_channel = val;
	ret = si4455_do_work(&s->port);

	return ret ? ret : count;
}

/*
 * tx_channel: rw sysfs entry.
 * Sets or returns the transmit channel index.
 * The new value will be used on next data transmit.
 */
static DEVICE_ATTR_RW(tx_channel);

static ssize_t tx_timeout_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct si4455_port *s = dev_get_drvdata(dev);

	return sprintf(buf, "%u\n", s->tx_wd_timeout);
}

static ssize_t tx_timeout_store(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct si4455_port *s = dev_get_drvdata(dev);
	unsigned long val;
	int ret;

	ret = kstrtoul(buf, 10, &val);
	if (ret)
		return ret;

	s->tx_wd_timeout = val;
	ret = si4455_do_work(&s->port);

	return ret ? ret : count;
}

/*
 * tx_timeout: rw sysfs entry.
 * Sets or returns the transmit timeout.
 * The new value will be used on next data transmit.
 */
static DEVICE_ATTR_RW(tx_timeout);

static ssize_t current_rssi_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct si4455_port *s = dev_get_drvdata(dev);

	return sprintf(buf, "%u\n", s->current_rssi);
}

/*
 * current_rssi: ro sysfs entry.
 * Returns the latest rssi value measured by chip.
 */
static DEVICE_ATTR_RO(current_rssi);

static struct attribute *si4455_attributes[] = {
	&dev_attr_package_size.attr,
	&dev_attr_rx_channel.attr,
	&dev_attr_tx_channel.attr,
	&dev_attr_tx_timeout.attr,
	&dev_attr_current_rssi.attr,
	NULL
};

static const struct attribute_group si4455_attr_group = {
	.attrs = si4455_attributes,
};

static int si4455_probe(struct device *dev,
			int irq)
{
	int ret;
	struct si4455_port *s;
	const void *of_ptr;
	const struct firmware *ez_fw = NULL;
	int line;

	/* Alloc port structure */
	s = devm_kzalloc(dev, sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;

	dev_set_drvdata(dev, s);
	mutex_init(&s->mutex);

	/* Alloc port line */
	line = find_first_zero_bit(si4455_port_lines, SI4455_UART_NRMAX);
	if (line == SI4455_UART_NRMAX) {
		dev_err(dev, "Unable to reguest port line index\n");
		ret = -ERANGE;
		goto out_generic;
	}

	s->shdn_gpio = devm_gpiod_get(dev, "shutdown", GPIOD_OUT_HIGH);
	if (IS_ERR(s->shdn_gpio)) {
		dev_err(dev, "Unable to reguest shdn gpio\n");
		ret = -EINVAL;
		goto out_generic;
	}

	of_ptr = of_get_property(dev->of_node, "silabs,package-size", NULL);
	if (IS_ERR_OR_NULL(of_ptr)) {
		dev_err(dev, "dt silabs,package-size property not present\n");
		ret = -EINVAL;
		goto out_generic;
	}
	s->package_size = be32_to_cpup(of_ptr);
	if (s->package_size > SI4455_FIFO_SIZE) {
		dev_err(dev, "dt silabs,package-size property maximum is %i\n", SI4455_FIFO_SIZE);
		ret = -EINVAL;
		goto out_generic;
	}

	of_ptr = of_get_property(dev->of_node, "silabs,tx-channel", NULL);
	if (IS_ERR_OR_NULL(of_ptr)) {
		dev_err(dev, "dt silabs,tx-channel property not present\n");
		ret = -EINVAL;
		goto out_generic;
	}
	s->tx_channel = be32_to_cpup(of_ptr);

	of_ptr = of_get_property(dev->of_node, "silabs,rx-channel", NULL);
	if (IS_ERR_OR_NULL(of_ptr)) {
		dev_err(dev, "dt silabs,rx-channel property not present\n");
		ret = -EINVAL;
		goto out_generic;
	}
	s->rx_channel = be32_to_cpup(of_ptr);

	of_ptr = of_get_property(dev->of_node, "silabs,tx-timeout-ms", NULL);
	if (IS_ERR_OR_NULL(of_ptr)) {
		s->tx_wd_timeout = 100;
		dev_warn(dev, "dt silabs,tx-timeout-ms property not present\n");
	} else {
		s->tx_wd_timeout = be32_to_cpup(of_ptr);
	}

	of_ptr = of_get_property(dev->of_node, "firmware-name", NULL);
	if (IS_ERR_OR_NULL(of_ptr)) {
		dev_err(dev, "dt firmware-name property not present\n");
		ret = -EINVAL;
		goto out_generic;
	}
	strncpy(s->ez_fw_name, of_ptr, sizeof(s->ez_fw_name) - 1);

	/* Initialize port data */
	s->port.dev		= dev;
	s->port.line		= line;
	s->port.irq		= irq;
	s->port.type		= PORT_SI4455;
	s->port.fifosize	= SI4455_FIFO_SIZE;
	s->port.flags		= UPF_FIXED_TYPE | UPF_LOW_LATENCY;
	s->port.iotype		= UPIO_PORT;
	s->port.iobase		= 1;
	s->port.ops		= &si4455_ops;

	si4455_s_power(dev, true);

	ret = si4455_get_part_info(&s->port, &s->part_info);
	dev_dbg(dev, "si4455_get_part_info() = %i\n", ret);
	if (ret == 0) {
		dev_info(dev, "partInfo.chip_rev = %u\n", s->part_info.chip_rev);
		dev_info(dev, "partInfo.part = %u\n", s->part_info.part);
		dev_info(dev, "partInfo.pbuild = %u\n", s->part_info.pbuild);
		dev_info(dev, "partInfo.id = %u\n", s->part_info.id);
		dev_info(dev, "partInfo.customer = %u\n", s->part_info.customer);
		dev_info(dev, "partInfo.rom_id = %u\n", s->part_info.rom_id);
		dev_info(dev, "partInfo.bond = %u\n", s->part_info.bond);
		if (s->part_info.part != 0x5544) {
			dev_err(dev, "unknown part(%u) error\n", s->part_info.part);
			ret = -ENODEV;
		}
	}

	if (ret)
		goto out_generic;

	ret = request_firmware(&ez_fw, s->ez_fw_name, dev);
	if (ret) {
		dev_err(dev, "firmware(%s) request error (%i)\n", s->ez_fw_name, ret);
		ret = -EINVAL;
		goto out_generic;
	}

	ret = si4455_re_configure(&s->port, ez_fw);
	release_firmware(ez_fw);
	if (ret) {
		dev_err(dev, "device configuration error (%i)\n", ret);
		ret = -EINVAL;
		goto out_generic;
	}

	ret = si4455_change_state(&s->port, SI4455_CMD_CHANGE_STATE_STATE_SLEEP);
	if (ret) {
		dev_err(dev, "device change state error (%i)\n", ret);
		goto out_generic;
	}

	/* Initialize queue for start TX */
	INIT_WORK(&s->tx_work, si4455_tx_proc);
	/* Initialize queue for start TX watchdog */
	INIT_WORK(&s->tx_wd_work, si4455_tx_wd_proc);
	/* Initialize queue for cts watchdog */
	INIT_WORK(&s->cts_wd_work, si4455_cts_wd_proc);
	/* Initialize timer for protecting and recovering tx_pending */
	timer_setup(&s->tx_wd_timer, si4455_tx_wd_event, 0);
	/* Initialize timer for recovering interface */
	timer_setup(&s->cts_wd_timer, si4455_cts_wd_event, 0);

	/* Register port */
	ret = uart_add_one_port(&si4455_uart, &s->port);
	if (ret) {
		s->port.dev = NULL;
		goto out_uart;
	}

	set_bit(line, si4455_port_lines);
	s->port.line = line;

	ret = sysfs_create_group(&dev->kobj, &si4455_attr_group);
	if (ret) {
		dev_err(dev, "sysfs_create_group error (%i)\n", ret);
		goto out_uart;
	}

	si4455_debugfs_init(dev);

	/* Setup interrupt */
	ret = devm_request_threaded_irq(dev, irq, NULL, si4455_ist,
					IRQF_ONESHOT | IRQF_SHARED,
					dev_name(dev), s);
	if (!ret)
		return 0;

	dev_err(dev, "Unable to reguest IRQ %i\n", irq);
	sysfs_remove_group(&dev->kobj, &si4455_attr_group);

out_uart:
	uart_remove_one_port(&si4455_uart, &s->port);
	clear_bit(line, si4455_port_lines);
out_generic:
	mutex_destroy(&s->mutex);
	si4455_s_power(dev, false);

	return ret;
}

static int si4455_remove(struct device *dev)
{
	struct si4455_port *s = dev_get_drvdata(dev);
	int line = s->port.line;

	cancel_work_sync(&s->tx_work);
	sysfs_remove_group(&dev->kobj, &si4455_attr_group);
	si4455_debugfs_clear(dev);
	uart_remove_one_port(&si4455_uart, &s->port);
	mutex_destroy(&s->mutex);
	clear_bit(line, si4455_port_lines);

	return 0;
}

static const struct of_device_id __maybe_unused si4455_dt_ids[] = {
	{ .compatible = "silabs,si4455" },
	{ }
};
MODULE_DEVICE_TABLE(of, si4455_dt_ids);

static int si4455_spi_probe(struct spi_device *spi)
{
	int ret;
	const struct of_device_id *of_id;

	/* Setup SPI bus */
	spi->bits_per_word	= 8;
	spi->mode		= SPI_MODE_0;
	ret = spi_setup(spi);
	if (ret)
		return ret;

	if (spi->dev.of_node) {
		of_id = of_match_device(si4455_dt_ids, &spi->dev);
		if (!of_id)
			return -ENODEV;
	}

	mutex_lock(&si4455_ports_lock);
	ret = si4455_probe(&spi->dev, spi->irq);
	mutex_unlock(&si4455_ports_lock);
	return ret;
}

static int si4455_spi_remove(struct spi_device *spi)
{
	int ret;

	mutex_lock(&si4455_ports_lock);
	ret = si4455_remove(&spi->dev);
	mutex_unlock(&si4455_ports_lock);
	return ret;
}

static struct spi_driver si4455_spi_driver = {
	.driver = {
		.name		= SI4455_NAME,
		.of_match_table	= of_match_ptr(si4455_dt_ids),
		.pm		= &si4455_pm_ops,
	},
	.probe			= si4455_spi_probe,
	.remove			= si4455_spi_remove,
};

static int __init si4455_uart_init(void)
{
	int ret;

	bitmap_zero(si4455_port_lines, SI4455_UART_NRMAX);

	ret = uart_register_driver(&si4455_uart);
	if (ret)
		return ret;

	return spi_register_driver(&si4455_spi_driver);
}
module_init(si4455_uart_init);

static void __exit si4455_uart_exit(void)
{
	spi_unregister_driver(&si4455_spi_driver);
	uart_unregister_driver(&si4455_uart);
}
module_exit(si4455_uart_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jozsef Horvath <info@ministro.hu>");
MODULE_DESCRIPTION("Si4455 serial driver");
