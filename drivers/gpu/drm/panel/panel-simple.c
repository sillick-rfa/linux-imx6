/*
 * Copyright (C) 2013, NVIDIA Corporation.  All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sub license,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <linux/backlight.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <video/of_videomode.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/spi/spi.h>

#include <drm/drmP.h>
#include <drm/drm_crtc.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_panel.h>

#include <video/display_timing.h>
#include <video/videomode.h>
#include <dt-bindings/display/simple_panel_mipi_cmds.h>

struct panel_desc {
	const struct drm_display_mode *modes;
	unsigned int num_modes;
	const struct display_timing *timings;
	unsigned int num_timings;

	unsigned int bpc;

	/**
	 * @width: width (in millimeters) of the panel's active display area
	 * @height: height (in millimeters) of the panel's active display area
	 */
	struct {
		unsigned int width;
		unsigned int height;
	} size;

	/**
	 * @prepare: the time (in milliseconds) that it takes for the panel to
	 *           become ready and start receiving video data
	 * @enable: the time (in milliseconds) that it takes for the panel to
	 *          display the first valid frame after starting to receive
	 *          video data
	 * @disable: the time (in milliseconds) that it takes for the panel to
	 *           turn the display off (no content is visible)
	 * @unprepare: the time (in milliseconds) that it takes for the panel
	 *             to power itself down completely
	 */
	struct {
		unsigned int power_up;
		unsigned int prepare;
		unsigned int enable;
		unsigned int disable;
		unsigned int unprepare;
		unsigned int power_down;
	} delay;

	u32 bus_format;
	u32 bus_flags;
};

struct cmds {
	const u8* cmds;
	unsigned length;
};

struct interface_cmds {
	struct cmds i2c;
	struct cmds mipi;
	struct cmds spi;
};

struct panel_simple {
	struct drm_panel base;
	bool prepared;
	bool enabled;

	const struct panel_desc *desc;
	struct panel_desc dt_desc;
	struct drm_display_mode dt_mode;

	struct backlight_device *backlight;
	struct regulator *supply;
	struct i2c_adapter *ddc;

	struct gpio_desc *gpd_power_enable;
	struct gpio_desc *gpd_prepare_enable;
	struct gpio_desc *reset;
	struct videomode vm;
	struct spi_device *spi;
	unsigned spi_max_frequency;
	struct i2c_adapter *i2c;
	unsigned i2c_max_frequency;
	unsigned i2c_address;
	unsigned char spi_9bit;
	unsigned spi_bits;
	struct interface_cmds cmds_init;
	struct interface_cmds cmds_enable;
	struct interface_cmds cmds_disable;
	/* Keep a mulitple of 9 */
	unsigned char tx_buf[63] __attribute__((aligned(64)));
	unsigned char rx_buf[63] __attribute__((aligned(64)));
};

static inline struct panel_simple *to_panel_simple(struct drm_panel *panel)
{
	return container_of(panel, struct panel_simple, base);
}

static int spi_send(struct panel_simple *panel, int rx)
{
	struct mipi_dsi_device *dsi;
	u8 *p = panel->tx_buf;
	struct spi_transfer t;
	struct spi_message m;
	int ret;

	if (!panel->spi || !panel->spi_bits)
		return 0;
	memset(&t, 0, sizeof(t));
	t.speed_hz = panel->spi_max_frequency;
	t.tx_buf = panel->tx_buf;
	t.rx_buf = (rx) ? panel->rx_buf : NULL;
	t.len = (panel->spi_bits + 7) >> 3;

	panel->spi_bits = 0;
	spi_message_init_with_transfers(&m, &t, 1);
	ret = spi_sync(panel->spi, &m);
	dsi = container_of(panel->base.dev, struct mipi_dsi_device, dev);
	if (ret) {
		dev_err(&dsi->dev,
			"spi(%d), (%d)%02x %02x %02x %02x %02x %02x\n",
			ret, t.len, p[0], p[1], p[2], p[3], p[4], p[5]);
	} else {
		pr_debug("spi(%d), (%d)%02x %02x %02x %02x %02x %02x  "
			"%02x %02x %02x %02x %02x %02x\n",
			ret, t.len, p[0], p[1], p[2], p[3], p[4], p[5],
			p[6], p[7], p[8], p[9], p[10], p[11]);
	}
	return ret;
}

static int store_9bit(struct panel_simple *panel, const u8 *p, unsigned l)
{
	int i = 0;
	u8 * buf = panel->tx_buf;
	unsigned val = 0;
	int bits = l * 9;
	int v_bits;
	int ret = 0;

	i = panel->spi_bits;
	if ((i + bits) > sizeof(panel->tx_buf) * 8) {
		ret = spi_send(panel, 0);
		i = 0;
		if (bits > sizeof(panel->tx_buf) * 8) {
			struct mipi_dsi_device *dsi;

			dsi = container_of(panel->base.dev, struct mipi_dsi_device, dev);
			dev_err(&dsi->dev, "too many bytes (%d)\n", l);
			return -EINVAL;
		}
	}

	panel->spi_bits = i + bits;
	buf += i >> 3;
	v_bits = (i & 7);
	if (v_bits) {
		bits += v_bits;
		val = *buf;
		val >>= (8 - v_bits);
	}
	val = (val << 9);
	v_bits += 9;
	if (l) {
		val |= *p++;
		l--;
	}
	while (bits > 0) {
		*buf++ = val >> (v_bits - 8);
		bits -= 8;
		v_bits -= 8;
		if (v_bits < 8) {
			val = (val << 9);
			v_bits += 9;
			if (l) {
				val |= 0x100;
				val |= *p++;
				l--;
			}
		}
	}
	return ret;
}

static int store_high(struct panel_simple *panel, int bits)
{
	int i;
	u8 * buf = panel->tx_buf;
	unsigned val = 0;
	int v_bits;

	i = panel->spi_bits;
	if ((i + bits) > sizeof(panel->tx_buf) * 8) {
		struct mipi_dsi_device *dsi;

		dsi = container_of(panel->base.dev, struct mipi_dsi_device, dev);
		dev_err(&dsi->dev, "too many bits (%d)\n", bits);
		return -EINVAL;
	}

	panel->spi_bits = i + bits;

	buf += i >> 3;
	v_bits = (i & 7);
	if (v_bits) {
		bits += v_bits;
		val = *buf;
		val >>= (8 - v_bits);
	}

	while (bits > 0) {
		if (v_bits < 8) {
			val = (val << 24) | 0xffffff;
			v_bits += 24;
		}
		*buf++ = val >> (v_bits - 8);
		bits -= 8;
		v_bits -= 8;
	}
	return 0;
}

static void extract_data(u8 *dst, unsigned bytes, u8 * buf, unsigned start_bit)
{
	unsigned val = 0;
	int v_bits;

	buf += start_bit >> 3;
	v_bits = (start_bit & 7);
	if (v_bits) {
		val = *buf++;
		v_bits = 8 - v_bits;
	}
	while (bytes > 0) {
		if (v_bits < 8) {
			val = (val << 8) | *buf++;
			v_bits += 8;
		}
		*dst++ = val >> (v_bits - 8);
		v_bits -= 8;
		bytes--;
	}
}
int simple_i2c_write(struct panel_simple *panel, const u8 *tx, unsigned tx_len)
{
	u8 *buf = panel->tx_buf;
	u8 tmp;
	struct i2c_msg msg;
	int ret;

	if (tx_len > sizeof(panel->tx_buf))
		return -EINVAL;
	memcpy(buf, tx, tx_len);
	if (tx_len >= 2) {
		tmp = buf[0];
		buf[0] = buf[1];
		buf[1] = tmp;
	}

	msg.addr = panel->i2c_address;
	msg.flags = 0;
	msg.len = tx_len;
	msg.buf = buf;
	ret = i2c_transfer(panel->i2c, &msg, 1);
	if (ret < 0) {
		msleep(10);
		ret = i2c_transfer(panel->i2c, &msg, 1);
	}
	return ret < 0 ? ret : 0;
}

int simple_i2c_read(struct panel_simple *panel, const u8 *tx, int tx_len, u8 *rx, unsigned rx_len)
{
	u8 *buf = panel->tx_buf;
	u8 tmp;
	struct i2c_msg msgs[2];
	int ret;

	if (tx_len > sizeof(panel->tx_buf))
		return -EINVAL;
	if (rx_len > sizeof(panel->rx_buf))
		return -EINVAL;
	memcpy(buf, tx, tx_len);
	if (tx_len >= 2) {
		tmp = buf[0];
		buf[0] = buf[1];
		buf[1] = tmp;
	}
	msgs[0].addr = panel->i2c_address;
	msgs[0].flags = 0;
	msgs[0].len = tx_len;
	msgs[0].buf = buf;

	msgs[1].addr = panel->i2c_address;
	msgs[1].flags = I2C_M_RD;
	msgs[1].len = rx_len;
	msgs[1].buf = rx;
	ret = i2c_transfer(panel->i2c, msgs, 2);
	return ret < 0 ? ret : 0;
}

#define TYPE_MIPI	0
#define TYPE_I2C	1
#define TYPE_SPI	2

static int send_cmd_list(struct panel_simple *panel, struct cmds *mc, int type, const unsigned char *id)
{
	struct drm_display_mode *dm = &panel->dt_mode;
	struct mipi_dsi_device *dsi;
	const u8 *cmd = mc->cmds;
	unsigned length = mc->length;
	u8 data[8];
	u8 cmd_buf[32];
	const u8 *p;
	unsigned l;
	unsigned len;
	unsigned mask;
	int ret;
	int generic;
	int match = 0;
	u64 readval, matchval;
	int skip = 0;
	int match_index;
	int i;

	pr_debug("%s:%d %d\n", __func__, length, type);
	if (!cmd || !length)
		return 0;

	panel->spi_bits = 0;
	dsi = container_of(panel->base.dev, struct mipi_dsi_device, dev);
	dsi->mode_flags |= MIPI_DSI_MODE_LPM;
	while (1) {
		len = *cmd++;
		length--;
		if (!length)
			break;
		if ((len >= S_IF_1_LANE) && (len <= S_IF_4_LANES)) {
			int lane_match = 1 + len - S_IF_1_LANE;

			if (lane_match != dsi->lanes)
				skip = 1;
			continue;
		}
		generic = len & 0x80;
		len &= 0x7f;

		p = cmd;
		l = len;
		ret = 0;
		if ((len < S_DELAY) || (len == S_DCS_LENGTH)
				|| (len == S_DCS_BUF)) {
			if (len == S_DCS_LENGTH) {
				len = *cmd++;
				length--;
				p = cmd;
				l = len;
			} else if (len == S_DCS_BUF) {
				l = *cmd++;
				length--;
				if (l > 32)
					l = 32;
				p = cmd_buf;
				len = 0;
			} else {
				p = cmd;
				l = len;
			}
			if (length < len) {
				dev_err(&dsi->dev, "Unexpected end of data\n");
				break;
			}
			if (!skip) {
				if (type == TYPE_I2C) {
					ret = simple_i2c_write(panel, p, l);
				} else if (type == TYPE_SPI) {
					if (panel->spi_9bit) {
						ret = store_9bit(panel, p, l);
					} else {
						if (l < sizeof(panel->tx_buf)) {
							memcpy(panel->tx_buf, p, l);
							panel->spi_bits = l * 8;
							ret = spi_send(panel, 0);
						} else {
							ret = -EINVAL;
						}
					}
				} else if (generic) {
					ret = mipi_dsi_generic_write(dsi, p, l);
				} else {
					ret = mipi_dsi_dcs_write_buffer(dsi, p, l);
				}
			}
		} else if (len == S_MRPS) {
			if (type == TYPE_MIPI) {
				ret = mipi_dsi_set_maximum_return_packet_size(
					dsi, cmd[0]);
			}
			l = len = 1;
		} else if ((len >= S_DCS_READ1) && (len <= S_DCS_READ8)) {
			data[0] = data[1] = data[2] = data[3] = 0;
			data[4] = data[5] = data[6] = data[7] = 0;
			len = len - S_DCS_READ1 + 1;
			match_index = generic ? 2 : 1;
			if (!skip) {
				if (type == TYPE_I2C) {
					ret = simple_i2c_read(panel, cmd, match_index, data, len);
				} else if (type == TYPE_SPI) {
					if (panel->spi_9bit) {
						spi_send(panel, 0);
						store_9bit(panel, cmd, match_index);
					} else {
						memcpy(panel->tx_buf, cmd, match_index);
						panel->spi_bits = match_index * 8;
					}
					l = panel->spi_bits;
					store_high(panel, len * 8);
					ret = spi_send(panel, 1);
					extract_data(data, len, panel->rx_buf, l);
				} else if (generic) {
					ret =  mipi_dsi_generic_read(dsi, cmd, 2, data, len);
					/* if an error is pending before BTA, an error report can happen */
					if (ret == -EPROTO)
						ret =  mipi_dsi_generic_read(dsi, cmd, 2, data, len);
					ret = 0;
				} else {
					ret =  mipi_dsi_dcs_read(dsi, cmd[0], data, len);
					if (ret == -EPROTO)
						ret =  mipi_dsi_dcs_read(dsi, cmd[0], data, len);
				}
				readval = 0;
				matchval = 0;
				for (i = 0; i < len; i++) {
					readval |= ((u64)data[i]) << (i << 3);
					matchval |= ((u64)cmd[match_index + i]) << (i << 3);
				}
				if (generic) {
					pr_debug("Read (%d)%s GEN: (%04x) 0x%llx cmp 0x%llx\n",
						ret, id, cmd[0] + (cmd[1] << 8), readval, matchval);
				} else {
					pr_debug("Read (%d)%s DCS: (%02x) 0x%llx cmp 0x%llx\n",
						ret, id, cmd[0], readval, matchval);
				}
				if (readval != matchval)
					match = -EINVAL;
			}
			l = len = len + match_index;
		} else if (len == S_DELAY) {
			if (!skip) {
				if (type == TYPE_SPI)
					spi_send(panel, 0);
				msleep(cmd[0]);
			}
			len = 1;
			if (length <= len)
				break;
			cmd += len;
			length -= len;
			l = len = 0;
		} else if ((len >= S_CONST) && (len <= S_VFP)) {
			int scmd, dest_start, dest_len, src_start;
			unsigned val;

			scmd = len;
			dest_start = cmd[0];
			dest_len = cmd[1];
			if (scmd == S_CONST) {
				val = cmd[2] | (cmd[3] << 8) | (cmd[4] << 16) | (cmd[5] << 24);
				len = 6;
				src_start = 0;
			} else {
				src_start = cmd[2];
				len = 3;
				switch (scmd) {
				case S_HSYNC:
					val = dm->hsync_end - dm->hsync_start;
					break;
				case S_HBP:
					val = dm->htotal - dm->hsync_end;
					break;
				case S_HACTIVE:
					val = dm->hdisplay;
					break;
				case S_HFP:
					val = dm->hsync_start - dm->hdisplay;
					break;
				case S_VSYNC:
					val = dm->vsync_end - dm->vsync_start;
					break;
				case S_VBP:
					val = dm->vtotal - dm->vsync_end;
					break;
				case S_VACTIVE:
					val = dm->vdisplay;
					break;
				case S_VFP:
					val = dm->vsync_start - dm->vdisplay;
					break;
				default:
					dev_err(&dsi->dev, "Unknown scmd 0x%x0x\n", scmd);
					val = 0;
					break;
				}
			}
			val >>= src_start;
			while (dest_len && dest_start < 256) {
				l = dest_start & 7;
				mask = (dest_len < 8) ? ((1 << dest_len) - 1) : 0xff;
				cmd_buf[dest_start >> 3] &= ~(mask << l);
				cmd_buf[dest_start >> 3] |= (val << l);
				l = 8 - l;
				dest_start += l;
				val >>= l;
				if (dest_len >= l)
					dest_len -= l;
				else
					dest_len = 0;
			}
			l = 0;
		} else {
			dev_err(&dsi->dev, "Unknown DCS command 0x%x 0x%x\n", cmd[-1], cmd[0]);
			match = -EINVAL;
			break;
		}
		if (ret < 0) {
			if (l >= 6) {
				dev_err(&dsi->dev,
					"Failed to send %s (%d), (%d)%02x %02x: %02x %02x %02x %02x\n",
					id, ret, l, p[0], p[1], p[2], p[3], p[4], p[5]);
			} else if (l >= 2) {
				dev_err(&dsi->dev,
					"Failed to send %s (%d), (%d)%02x %02x\n",
					id, ret, l, p[0], p[1]);
			} else {
				dev_err(&dsi->dev,
					"Failed to send %s (%d), (%d)%02x\n",
					id, ret, l, p[0]);
			}
			return ret;
		} else {
			if (!skip) {
				if (l >= 18) {
					pr_debug("Sent %s (%d), (%d)%02x %02x: %02x %02x %02x %02x"
							"  %02x %02x %02x %02x"
							"  %02x %02x %02x %02x"
							"  %02x %02x %02x %02x\n",
						id, ret, l, p[0], p[1], p[2], p[3], p[4], p[5],
						p[6], p[7], p[8], p[9],
						p[10], p[11], p[12], p[13],
						p[14], p[15], p[16], p[17]);
				} else if (l >= 6) {
					pr_debug("Sent %s (%d), (%d)%02x %02x: %02x %02x %02x %02x\n",
						id, ret, l, p[0], p[1], p[2], p[3], p[4], p[5]);
				} else if (l >= 2) {
					pr_debug("Sent %s (%d), (%d)%02x %02x\n",
						id, ret, l, p[0], p[1]);
				} else if (l) {
					pr_debug("Sent %s (%d), (%d)%02x\n",
						id, ret, l, p[0]);
				}
			}
		}
		if (length < len) {
			dev_err(&dsi->dev, "Unexpected end of data\n");
			break;
		}
		cmd += len;
		length -= len;
		if (!length)
			break;
		skip = 0;
	}
	if (!match && type == TYPE_SPI)
		match = spi_send(panel, 0);
	return match;
};

static int send_all_cmd_lists(struct panel_simple *panel, struct interface_cmds *msc)
{
	int ret = 0;

	if (panel->i2c)
		ret = send_cmd_list(panel,  &msc->i2c, TYPE_I2C, "i2c");
	if (!ret)
		ret = send_cmd_list(panel,  &msc->mipi, TYPE_MIPI, "mipi");
	if (!ret && panel->spi)
		ret = send_cmd_list(panel,  &msc->spi, TYPE_SPI, "spi");
	return ret;
}

static int panel_simple_get_fixed_modes(struct panel_simple *panel)
{
	struct drm_connector *connector = panel->base.connector;
	struct drm_device *drm = panel->base.drm;
	struct drm_display_mode *mode;
	unsigned int i, num = 0;

	if (!panel->desc)
		return 0;

	for (i = 0; i < panel->desc->num_timings; i++) {
		const struct display_timing *dt = &panel->desc->timings[i];
		struct videomode vm;

		videomode_from_timing(dt, &vm);
		mode = drm_mode_create(drm);
		if (!mode) {
			dev_err(drm->dev, "failed to add mode %ux%u\n",
				dt->hactive.typ, dt->vactive.typ);
			continue;
		}

		drm_display_mode_from_videomode(&vm, mode);

		mode->type |= DRM_MODE_TYPE_DRIVER;

		if (panel->desc->num_timings == 1)
			mode->type |= DRM_MODE_TYPE_PREFERRED;

		drm_mode_probed_add(connector, mode);
		num++;
	}

	for (i = 0; i < panel->desc->num_modes; i++) {
		const struct drm_display_mode *m = &panel->desc->modes[i];

		mode = drm_mode_duplicate(drm, m);
		if (!mode) {
			dev_err(drm->dev, "failed to add mode %ux%u@%u\n",
				m->hdisplay, m->vdisplay, m->vrefresh);
			continue;
		}

		mode->type |= DRM_MODE_TYPE_DRIVER;

		if (panel->desc->num_modes == 1)
			mode->type |= DRM_MODE_TYPE_PREFERRED;

		drm_mode_set_name(mode);

		drm_mode_probed_add(connector, mode);
		num++;
	}

	connector->display_info.bpc = panel->desc->bpc;
	connector->display_info.width_mm = panel->desc->size.width;
	connector->display_info.height_mm = panel->desc->size.height;
	if (panel->desc->bus_format)
		drm_display_info_set_bus_formats(&connector->display_info,
						 &panel->desc->bus_format, 1);
	connector->display_info.bus_flags = panel->desc->bus_flags;

	return num;
}

static int panel_simple_disable(struct drm_panel *panel)
{
	struct panel_simple *p = to_panel_simple(panel);

	if (!p->enabled)
		return 0;

	if (p->backlight) {
		p->backlight->props.power = FB_BLANK_POWERDOWN;
		p->backlight->props.state |= BL_CORE_FBBLANK;
		backlight_update_status(p->backlight);
	}

	if (p->desc->delay.disable)
		msleep(p->desc->delay.disable);
	send_all_cmd_lists(p, &p->cmds_disable);

	p->enabled = false;

	return 0;
}

static int panel_simple_unprepare(struct drm_panel *panel)
{
	struct panel_simple *p = to_panel_simple(panel);

	if (!p->prepared)
		return 0;

	if (p->desc->delay.unprepare)
		msleep(p->desc->delay.unprepare);
	if (p->reset)
		gpiod_set_value_cansleep(p->reset, 1);
	if (p->gpd_prepare_enable)
		gpiod_set_value_cansleep(p->gpd_prepare_enable, 0);

	regulator_disable(p->supply);


	p->prepared = false;

	return 0;
}

static int panel_simple_power_down(struct drm_panel *panel)
{
	struct panel_simple *p = to_panel_simple(panel);

	if (p->desc->delay.power_down)
		msleep(p->desc->delay.power_down);
	if (p->gpd_power_enable)
		gpiod_set_value_cansleep(p->gpd_power_enable, 0);
	return 0;
}

static int panel_simple_power_up(struct drm_panel *panel)
{
	struct panel_simple *p = to_panel_simple(panel);

	if (p->gpd_power_enable)
		gpiod_set_value_cansleep(p->gpd_power_enable, 1);
	if (p->desc->delay.power_up)
		msleep(p->desc->delay.power_up);
	return 0;
}

static int panel_simple_prepare(struct drm_panel *panel)
{
	struct panel_simple *p = to_panel_simple(panel);
	int err;

	if (p->prepared)
		return 0;

	err = regulator_enable(p->supply);
	if (err < 0) {
		dev_err(panel->dev, "failed to enable supply: %d\n", err);
		return err;
	}

	if (p->gpd_prepare_enable)
		gpiod_set_value_cansleep(p->gpd_prepare_enable, 1);
	if (p->reset)
		gpiod_set_value_cansleep(p->reset, 0);


	if (p->desc->delay.prepare)
		msleep(p->desc->delay.prepare);

	err = send_all_cmd_lists(p, &p->cmds_init);
	if (err) {
		regulator_disable(p->supply);
		return err;
	}
	p->prepared = true;

	return 0;
}

static int panel_simple_enable(struct drm_panel *panel)
{
	struct panel_simple *p = to_panel_simple(panel);
	int ret;

	if (p->enabled)
		return 0;

	ret = send_all_cmd_lists(p, &p->cmds_enable);
	if (ret < 0)
		goto fail;
	if (p->desc->delay.enable)
		msleep(p->desc->delay.enable);

	if (p->backlight) {
		p->backlight->props.state &= ~BL_CORE_FBBLANK;
		p->backlight->props.power = FB_BLANK_UNBLANK;
		backlight_update_status(p->backlight);
	}

	p->enabled = true;

	return 0;
fail:
	if (p->reset)
		gpiod_set_value_cansleep(p->reset, 1);
	if (p->gpd_prepare_enable)
		gpiod_set_value_cansleep(p->gpd_prepare_enable, 0);
	return ret;
}

static int panel_simple_get_modes(struct drm_panel *panel)
{
	struct panel_simple *p = to_panel_simple(panel);
	int num = 0;

	/* probe EDID if a DDC bus is available */
	if (p->ddc) {
		struct edid *edid = drm_get_edid(panel->connector, p->ddc);
		drm_mode_connector_update_edid_property(panel->connector, edid);
		if (edid) {
			num += drm_add_edid_modes(panel->connector, edid);
			kfree(edid);
		}
	}

	/* add hard-coded panel modes */
	num += panel_simple_get_fixed_modes(p);

	return num;
}

static int panel_simple_get_timings(struct drm_panel *panel,
				    unsigned int num_timings,
				    struct display_timing *timings)
{
	struct panel_simple *p = to_panel_simple(panel);
	unsigned int i;

	if (p->desc->num_timings < num_timings)
		num_timings = p->desc->num_timings;

	if (timings)
		for (i = 0; i < num_timings; i++)
			timings[i] = p->desc->timings[i];

	return p->desc->num_timings;
}

static const struct drm_panel_funcs panel_simple_funcs = {
	.disable = panel_simple_disable,
	.unprepare = panel_simple_unprepare,
	.power_down = panel_simple_power_down,
	.power_up = panel_simple_power_up,
	.prepare = panel_simple_prepare,
	.enable = panel_simple_enable,
	.get_modes = panel_simple_get_modes,
	.get_timings = panel_simple_get_timings,
};

void check_for_cmds(struct device_node *np, const char *dt_name, struct cmds *mc)
{
	void *data;
	int data_len;
	int ret;

	/* Check for mipi command arrays */
	if (!of_get_property(np, dt_name, &data_len) || !data_len)
		return;

	data = kmalloc(data_len, GFP_KERNEL);
	if (!data)
		return;

	ret = of_property_read_u8_array(np, dt_name, data, data_len);
	if (ret) {
		pr_info("failed to read %s from DT: %d\n",
			dt_name, ret);
		kfree(data);
		return;
	}
	mc->cmds = data;
	mc->length = data_len;
}

static void init_common(struct device_node *np, struct panel_desc *ds,
		struct drm_display_mode *dm, struct mipi_dsi_device *dsi)
{
	of_property_read_u32(np, "delay-power-up", &ds->delay.power_up);
	of_property_read_u32(np, "delay-prepare", &ds->delay.prepare);
	of_property_read_u32(np, "delay-enable", &ds->delay.enable);
	of_property_read_u32(np, "delay-disable", &ds->delay.disable);
	of_property_read_u32(np, "delay-unprepare", &ds->delay.unprepare);
	of_property_read_u32(np, "delay-power-down", &ds->delay.power_down);
	of_property_read_u32(np, "min-hs-clock-multiple", &dm->min_hs_clock_multiple);
	of_property_read_u32(np, "mipi-dsi-multiple", &dm->mipi_dsi_multiple);
	if (dsi) {
		if (of_property_read_bool(np, "mode-video-hfp-disable"))
			dsi->mode_flags |= MIPI_DSI_MODE_VIDEO_HFP;
		if (of_property_read_bool(np, "mode-video-hbp-disable"))
			dsi->mode_flags |= MIPI_DSI_MODE_VIDEO_HBP;
		if (of_property_read_bool(np, "mode-video-hsa-disable"))
			dsi->mode_flags |= MIPI_DSI_MODE_VIDEO_HSA;
	}
}

static int panel_simple_probe(struct device *dev, const struct panel_desc *desc,
		struct mipi_dsi_device *dsi)
{
	struct device_node *backlight, *ddc, *spi_node;
	struct device_node *i2c_node;
	struct spi_device *spi = NULL;
	struct i2c_adapter *i2c = NULL;
	struct panel_simple *panel;
	int err;

	panel = devm_kzalloc(dev, sizeof(*panel), GFP_KERNEL);
	if (!panel)
		return -ENOMEM;

	panel->enabled = false;
	panel->prepared = false;
	panel->desc = desc;
	if (!desc) {
		struct videomode vm;
		struct drm_display_mode *dm = &panel->dt_mode;
		const char *bf;
		struct panel_desc *ds = &panel->dt_desc;
		struct device_node *np = dev->of_node;
		struct device_node *cmds_np;
		u32 bridge_de_active;
		u32 bridge_sync_active;

		of_property_read_u32(np, "panel-width-mm", &ds->size.width);
		of_property_read_u32(np, "panel-height-mm", &ds->size.height);
		err = of_get_videomode(np, &vm, 0);
		if (of_property_read_u32(np, "bridge-de-active", &bridge_de_active)) {
			bridge_de_active = -1;
		}
		if (of_property_read_u32(np, "bridge-sync-active", &bridge_sync_active)) {
			bridge_sync_active = -1;
		}

		if (err < 0)
			return err;
		drm_display_mode_from_videomode(&vm, dm);
		if (vm.flags & DISPLAY_FLAGS_DE_HIGH)
			ds->bus_flags |= DRM_BUS_FLAG_DE_HIGH;
		if (vm.flags & DISPLAY_FLAGS_DE_LOW)
			ds->bus_flags |= DRM_BUS_FLAG_DE_LOW;
		if (bridge_de_active <= 1) {
			ds->bus_flags &= ~(DRM_BUS_FLAG_DE_HIGH |
					DRM_BUS_FLAG_DE_LOW);
			ds->bus_flags |= bridge_de_active ? DRM_BUS_FLAG_DE_HIGH
					: DRM_BUS_FLAG_DE_LOW;
		}

		if (bridge_sync_active <= 1) {
			dm->flags &= ~(
				DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC |
				DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC);
			dm->flags |= bridge_sync_active ?
				DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC :
				DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC;
		}
		if (vm.flags & DISPLAY_FLAGS_PIXDATA_NEGEDGE)
			ds->bus_flags |= DRM_BUS_FLAG_PIXDATA_NEGEDGE;
		if (vm.flags & DISPLAY_FLAGS_PIXDATA_POSEDGE)
			ds->bus_flags |= DRM_BUS_FLAG_PIXDATA_POSEDGE;
		dev_info(dev, "vm.flags=%x bus_flags=%x flags=%x\n", vm.flags, ds->bus_flags, dm->flags);

		err = of_property_read_string(np, "bus-format", &bf);
		if (err) {
			dev_err(dev, "bus-format missing %d\n", err);
			return err;
		}
		if (!strcmp(bf, "rgb888")) {
			ds->bus_format = MEDIA_BUS_FMT_RGB888_1X24;
		} else if (!strcmp(bf, "rgb666")) {
			ds->bus_format = MEDIA_BUS_FMT_RGB666_1X18;
		} else {
			dev_err(dev, "unknown bus-format %s\n", bf);
			return -EINVAL;
		}
		init_common(np, ds, dm, dsi);
		of_property_read_u32(np, "bits-per-color", &ds->bpc);
		ds->modes = dm;
		ds->num_modes = 1;
		panel->desc = ds;
		cmds_np = of_parse_phandle(np, "mipi-cmds", 0);
		if (cmds_np) {
			i2c_node = of_parse_phandle(cmds_np, "i2c-bus", 0);
			if (i2c_node) {
				i2c = of_find_i2c_adapter_by_node(i2c_node);
				of_node_put(i2c_node);

				if (!i2c) {
					pr_debug("%s:i2c deferred\n", __func__);
					return -EPROBE_DEFER;
				}
				panel->i2c = i2c;
			}

			spi_node = of_parse_phandle(cmds_np, "spi", 0);
			if (spi_node) {
				spi = of_find_spi_device_by_node(spi_node);
				of_node_put(spi_node);

				if (!spi) {
					pr_debug("%s:spi deferred\n", __func__);
					err = -EPROBE_DEFER;
					goto free_i2c;
				}
				panel->spi = spi;
			}

			if (i2c) {
				check_for_cmds(cmds_np, "i2c-cmds-init",
				       &panel->cmds_init.i2c);
				check_for_cmds(cmds_np, "i2c-cmds-enable",
				       &panel->cmds_enable.i2c);
				check_for_cmds(cmds_np, "i2c-cmds-disable",
				       &panel->cmds_disable.i2c);
				of_property_read_u32(cmds_np, "i2c-address", &panel->i2c_address);
				of_property_read_u32(cmds_np, "i2c-max-frequency", &panel->i2c_max_frequency);
			}
			check_for_cmds(cmds_np, "mipi-cmds-init",
				       &panel->cmds_init.mipi);
			check_for_cmds(cmds_np, "mipi-cmds-enable",
				       &panel->cmds_enable.mipi);
			check_for_cmds(cmds_np, "mipi-cmds-disable",
				       &panel->cmds_disable.mipi);

			if (spi) {
				if (of_property_read_bool(cmds_np, "spi-9-bit"))
					panel->spi_9bit = 1;
				check_for_cmds(cmds_np, "spi-cmds-init",
				       &panel->cmds_init.spi);
				check_for_cmds(cmds_np, "spi-cmds-enable",
				       &panel->cmds_enable.spi);
				check_for_cmds(cmds_np, "spi-cmds-disable",
				       &panel->cmds_disable.spi);
				of_property_read_u32(cmds_np, "spi-max-frequency", &panel->spi_max_frequency);
			}
			init_common(cmds_np, ds, dm, dsi);
		}
		pr_info("%s: delay %d %d, %d %d\n", __func__,
			ds->delay.prepare, ds->delay.enable,
			ds->delay.disable, ds->delay.unprepare);
	}

	panel->supply = devm_regulator_get(dev, "power");
	if (IS_ERR(panel->supply)) {
		err = PTR_ERR(panel->supply);
		goto free_spi;
	}

	panel->reset = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(panel->reset)) {
		err = PTR_ERR(panel->reset);
		dev_err(dev, "failed to request reset: %d\n", err);
		goto free_spi;
	}

	panel->gpd_prepare_enable = devm_gpiod_get_optional(dev, "prepare-enable",
						     GPIOD_OUT_LOW);
	if (IS_ERR(panel->gpd_prepare_enable)) {
		err = PTR_ERR(panel->gpd_prepare_enable);
		dev_err(dev, "failed to request GPIO: %d\n", err);
		goto free_spi;
	}

	panel->gpd_power_enable = devm_gpiod_get_optional(dev, "enable",
						     GPIOD_OUT_LOW);
	if (IS_ERR(panel->gpd_power_enable)) {
		err = PTR_ERR(panel->gpd_power_enable);
		dev_err(dev, "failed to request GPIO: %d\n", err);
		goto free_spi;
	}

	backlight = of_parse_phandle(dev->of_node, "backlight", 0);
	if (backlight) {
		panel->backlight = of_find_backlight_by_node(backlight);
		of_node_put(backlight);

		if (!panel->backlight) {
			err = -EPROBE_DEFER;
			goto free_spi;
		}
	}

	ddc = of_parse_phandle(dev->of_node, "ddc-i2c-bus", 0);
	if (ddc) {
		panel->ddc = of_find_i2c_adapter_by_node(ddc);
		of_node_put(ddc);

		if (!panel->ddc) {
			err = -EPROBE_DEFER;
			goto free_backlight;
		}
	}

	drm_panel_init(&panel->base);
	panel->base.dev = dev;
	panel->base.funcs = &panel_simple_funcs;

	err = drm_panel_add(&panel->base);
	if (err < 0)
		goto free_ddc;

	dev_set_drvdata(dev, panel);

	return 0;

free_ddc:
	if (panel->ddc)
		put_device(&panel->ddc->dev);
free_backlight:
	if (panel->backlight)
		put_device(&panel->backlight->dev);
free_spi:
	if (spi)
		put_device(&spi->dev);
free_i2c:
	if (i2c)
		put_device(&i2c->dev);

	return err;
}

static int panel_simple_remove(struct device *dev)
{
	struct panel_simple *panel = dev_get_drvdata(dev);

	drm_panel_detach(&panel->base);
	drm_panel_remove(&panel->base);

	panel_simple_disable(&panel->base);
	panel_simple_unprepare(&panel->base);

	if (panel->ddc)
		put_device(&panel->ddc->dev);

	if (panel->backlight)
		put_device(&panel->backlight->dev);
	if (panel->spi)
		put_device(&panel->spi->dev);
	if (panel->i2c)
		put_device(&panel->i2c->dev);

	return 0;
}

static void panel_simple_shutdown(struct device *dev)
{
	struct panel_simple *panel = dev_get_drvdata(dev);

	panel_simple_disable(&panel->base);
	panel_simple_unprepare(&panel->base);
}

static const struct drm_display_mode ampire_am_480272h3tmqw_t01h_mode = {
	.clock = 9000,
	.hdisplay = 480,
	.hsync_start = 480 + 2,
	.hsync_end = 480 + 2 + 41,
	.htotal = 480 + 2 + 41 + 2,
	.vdisplay = 272,
	.vsync_start = 272 + 2,
	.vsync_end = 272 + 2 + 10,
	.vtotal = 272 + 2 + 10 + 2,
	.vrefresh = 60,
	.flags = DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC,
};

static const struct panel_desc ampire_am_480272h3tmqw_t01h = {
	.modes = &ampire_am_480272h3tmqw_t01h_mode,
	.num_modes = 1,
	.bpc = 8,
	.size = {
		.width = 105,
		.height = 67,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X24,
};

static const struct drm_display_mode ampire_am800480r3tmqwa1h_mode = {
	.clock = 33333,
	.hdisplay = 800,
	.hsync_start = 800 + 0,
	.hsync_end = 800 + 0 + 255,
	.htotal = 800 + 0 + 255 + 0,
	.vdisplay = 480,
	.vsync_start = 480 + 2,
	.vsync_end = 480 + 2 + 45,
	.vtotal = 480 + 2 + 45 + 0,
	.vrefresh = 60,
	.flags = DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC,
};

static const struct panel_desc ampire_am800480r3tmqwa1h = {
	.modes = &ampire_am800480r3tmqwa1h_mode,
	.num_modes = 1,
	.bpc = 6,
	.size = {
		.width = 152,
		.height = 91,
	},
	.bus_format = MEDIA_BUS_FMT_RGB666_1X18,
};

static const struct drm_display_mode auo_b101aw03_mode = {
	.clock = 51450,
	.hdisplay = 1024,
	.hsync_start = 1024 + 156,
	.hsync_end = 1024 + 156 + 8,
	.htotal = 1024 + 156 + 8 + 156,
	.vdisplay = 600,
	.vsync_start = 600 + 16,
	.vsync_end = 600 + 16 + 6,
	.vtotal = 600 + 16 + 6 + 16,
	.vrefresh = 60,
};

static const struct panel_desc auo_b101aw03 = {
	.modes = &auo_b101aw03_mode,
	.num_modes = 1,
	.bpc = 6,
	.size = {
		.width = 223,
		.height = 125,
	},
};

static const struct drm_display_mode auo_b101ean01_mode = {
	.clock = 72500,
	.hdisplay = 1280,
	.hsync_start = 1280 + 119,
	.hsync_end = 1280 + 119 + 32,
	.htotal = 1280 + 119 + 32 + 21,
	.vdisplay = 800,
	.vsync_start = 800 + 4,
	.vsync_end = 800 + 4 + 20,
	.vtotal = 800 + 4 + 20 + 8,
	.vrefresh = 60,
};

static const struct panel_desc auo_b101ean01 = {
	.modes = &auo_b101ean01_mode,
	.num_modes = 1,
	.bpc = 6,
	.size = {
		.width = 217,
		.height = 136,
	},
};

static const struct drm_display_mode auo_b101xtn01_mode = {
	.clock = 72000,
	.hdisplay = 1366,
	.hsync_start = 1366 + 20,
	.hsync_end = 1366 + 20 + 70,
	.htotal = 1366 + 20 + 70,
	.vdisplay = 768,
	.vsync_start = 768 + 14,
	.vsync_end = 768 + 14 + 42,
	.vtotal = 768 + 14 + 42,
	.vrefresh = 60,
	.flags = DRM_MODE_FLAG_NVSYNC | DRM_MODE_FLAG_NHSYNC,
};

static const struct panel_desc auo_b101xtn01 = {
	.modes = &auo_b101xtn01_mode,
	.num_modes = 1,
	.bpc = 6,
	.size = {
		.width = 223,
		.height = 125,
	},
};

static const struct drm_display_mode auo_b116xw03_mode = {
	.clock = 70589,
	.hdisplay = 1366,
	.hsync_start = 1366 + 40,
	.hsync_end = 1366 + 40 + 40,
	.htotal = 1366 + 40 + 40 + 32,
	.vdisplay = 768,
	.vsync_start = 768 + 10,
	.vsync_end = 768 + 10 + 12,
	.vtotal = 768 + 10 + 12 + 6,
	.vrefresh = 60,
};

static const struct panel_desc auo_b116xw03 = {
	.modes = &auo_b116xw03_mode,
	.num_modes = 1,
	.bpc = 6,
	.size = {
		.width = 256,
		.height = 144,
	},
};

static const struct drm_display_mode auo_b133xtn01_mode = {
	.clock = 69500,
	.hdisplay = 1366,
	.hsync_start = 1366 + 48,
	.hsync_end = 1366 + 48 + 32,
	.htotal = 1366 + 48 + 32 + 20,
	.vdisplay = 768,
	.vsync_start = 768 + 3,
	.vsync_end = 768 + 3 + 6,
	.vtotal = 768 + 3 + 6 + 13,
	.vrefresh = 60,
};

static const struct panel_desc auo_b133xtn01 = {
	.modes = &auo_b133xtn01_mode,
	.num_modes = 1,
	.bpc = 6,
	.size = {
		.width = 293,
		.height = 165,
	},
};

static const struct drm_display_mode auo_b133htn01_mode = {
	.clock = 150660,
	.hdisplay = 1920,
	.hsync_start = 1920 + 172,
	.hsync_end = 1920 + 172 + 80,
	.htotal = 1920 + 172 + 80 + 60,
	.vdisplay = 1080,
	.vsync_start = 1080 + 25,
	.vsync_end = 1080 + 25 + 10,
	.vtotal = 1080 + 25 + 10 + 10,
	.vrefresh = 60,
};

static const struct panel_desc auo_b133htn01 = {
	.modes = &auo_b133htn01_mode,
	.num_modes = 1,
	.bpc = 6,
	.size = {
		.width = 293,
		.height = 165,
	},
	.delay = {
		.prepare = 105,
		.enable = 20,
		.unprepare = 50,
	},
};

static const struct display_timing auo_g133han01_timings = {
	.pixelclock = { 134000000, 141200000, 149000000 },
	.hactive = { 1920, 1920, 1920 },
	.hfront_porch = { 39, 58, 77 },
	.hback_porch = { 59, 88, 117 },
	.hsync_len = { 28, 42, 56 },
	.vactive = { 1080, 1080, 1080 },
	.vfront_porch = { 3, 8, 11 },
	.vback_porch = { 5, 14, 19 },
	.vsync_len = { 4, 14, 19 },
};

static const struct panel_desc auo_g133han01 = {
	.timings = &auo_g133han01_timings,
	.num_timings = 1,
	.bpc = 8,
	.size = {
		.width = 293,
		.height = 165,
	},
	.delay = {
		.prepare = 200,
		.enable = 50,
		.disable = 50,
		.unprepare = 1000,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X7X4_JEIDA,
};

static const struct display_timing auo_g185han01_timings = {
	.pixelclock = { 120000000, 144000000, 175000000 },
	.hactive = { 1920, 1920, 1920 },
	.hfront_porch = { 18, 60, 74 },
	.hback_porch = { 12, 44, 54 },
	.hsync_len = { 10, 24, 32 },
	.vactive = { 1080, 1080, 1080 },
	.vfront_porch = { 6, 10, 40 },
	.vback_porch = { 2, 5, 20 },
	.vsync_len = { 2, 5, 20 },
};

static const struct panel_desc auo_g185han01 = {
	.timings = &auo_g185han01_timings,
	.num_timings = 1,
	.bpc = 8,
	.size = {
		.width = 409,
		.height = 230,
	},
	.delay = {
		.prepare = 50,
		.enable = 200,
		.disable = 110,
		.unprepare = 1000,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X7X4_SPWG,
};

static const struct display_timing auo_p320hvn03_timings = {
	.pixelclock = { 106000000, 148500000, 164000000 },
	.hactive = { 1920, 1920, 1920 },
	.hfront_porch = { 25, 50, 130 },
	.hback_porch = { 25, 50, 130 },
	.hsync_len = { 20, 40, 105 },
	.vactive = { 1080, 1080, 1080 },
	.vfront_porch = { 8, 17, 150 },
	.vback_porch = { 8, 17, 150 },
	.vsync_len = { 4, 11, 100 },
};

static const struct panel_desc auo_p320hvn03 = {
	.timings = &auo_p320hvn03_timings,
	.num_timings = 1,
	.bpc = 8,
	.size = {
		.width = 698,
		.height = 393,
	},
	.delay = {
		.prepare = 1,
		.enable = 450,
		.unprepare = 500,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X7X4_JEIDA,
};

static const struct drm_display_mode auo_t215hvn01_mode = {
	.clock = 148800,
	.hdisplay = 1920,
	.hsync_start = 1920 + 88,
	.hsync_end = 1920 + 88 + 44,
	.htotal = 1920 + 88 + 44 + 148,
	.vdisplay = 1080,
	.vsync_start = 1080 + 4,
	.vsync_end = 1080 + 4 + 5,
	.vtotal = 1080 + 4 + 5 + 36,
	.vrefresh = 60,
};

static const struct panel_desc auo_t215hvn01 = {
	.modes = &auo_t215hvn01_mode,
	.num_modes = 1,
	.bpc = 8,
	.size = {
		.width = 430,
		.height = 270,
	},
	.delay = {
		.disable = 5,
		.unprepare = 1000,
	}
};

static const struct drm_display_mode avic_tm070ddh03_mode = {
	.clock = 51200,
	.hdisplay = 1024,
	.hsync_start = 1024 + 160,
	.hsync_end = 1024 + 160 + 4,
	.htotal = 1024 + 160 + 4 + 156,
	.vdisplay = 600,
	.vsync_start = 600 + 17,
	.vsync_end = 600 + 17 + 1,
	.vtotal = 600 + 17 + 1 + 17,
	.vrefresh = 60,
};

static const struct panel_desc avic_tm070ddh03 = {
	.modes = &avic_tm070ddh03_mode,
	.num_modes = 1,
	.bpc = 8,
	.size = {
		.width = 154,
		.height = 90,
	},
	.delay = {
		.prepare = 20,
		.enable = 200,
		.disable = 200,
	},
};

static const struct drm_display_mode boe_nv101wxmn51_modes[] = {
	{
		.clock = 71900,
		.hdisplay = 1280,
		.hsync_start = 1280 + 48,
		.hsync_end = 1280 + 48 + 32,
		.htotal = 1280 + 48 + 32 + 80,
		.vdisplay = 800,
		.vsync_start = 800 + 3,
		.vsync_end = 800 + 3 + 5,
		.vtotal = 800 + 3 + 5 + 24,
		.vrefresh = 60,
	},
	{
		.clock = 57500,
		.hdisplay = 1280,
		.hsync_start = 1280 + 48,
		.hsync_end = 1280 + 48 + 32,
		.htotal = 1280 + 48 + 32 + 80,
		.vdisplay = 800,
		.vsync_start = 800 + 3,
		.vsync_end = 800 + 3 + 5,
		.vtotal = 800 + 3 + 5 + 24,
		.vrefresh = 48,
	},
};

static const struct panel_desc boe_nv101wxmn51 = {
	.modes = boe_nv101wxmn51_modes,
	.num_modes = ARRAY_SIZE(boe_nv101wxmn51_modes),
	.bpc = 8,
	.size = {
		.width = 217,
		.height = 136,
	},
	.delay = {
		.prepare = 210,
		.enable = 50,
		.unprepare = 160,
	},
};

static const struct drm_display_mode chunghwa_claa070wp03xg_mode = {
	.clock = 66770,
	.hdisplay = 800,
	.hsync_start = 800 + 49,
	.hsync_end = 800 + 49 + 33,
	.htotal = 800 + 49 + 33 + 17,
	.vdisplay = 1280,
	.vsync_start = 1280 + 1,
	.vsync_end = 1280 + 1 + 7,
	.vtotal = 1280 + 1 + 7 + 15,
	.vrefresh = 60,
	.flags = DRM_MODE_FLAG_NVSYNC | DRM_MODE_FLAG_NHSYNC,
};

static const struct panel_desc chunghwa_claa070wp03xg = {
	.modes = &chunghwa_claa070wp03xg_mode,
	.num_modes = 1,
	.bpc = 6,
	.size = {
		.width = 94,
		.height = 150,
	},
};

static const struct drm_display_mode chunghwa_claa101wa01a_mode = {
	.clock = 72070,
	.hdisplay = 1366,
	.hsync_start = 1366 + 58,
	.hsync_end = 1366 + 58 + 58,
	.htotal = 1366 + 58 + 58 + 58,
	.vdisplay = 768,
	.vsync_start = 768 + 4,
	.vsync_end = 768 + 4 + 4,
	.vtotal = 768 + 4 + 4 + 4,
	.vrefresh = 60,
};

static const struct panel_desc chunghwa_claa101wa01a = {
	.modes = &chunghwa_claa101wa01a_mode,
	.num_modes = 1,
	.bpc = 6,
	.size = {
		.width = 220,
		.height = 120,
	},
};

static const struct drm_display_mode chunghwa_claa101wb01_mode = {
	.clock = 69300,
	.hdisplay = 1366,
	.hsync_start = 1366 + 48,
	.hsync_end = 1366 + 48 + 32,
	.htotal = 1366 + 48 + 32 + 20,
	.vdisplay = 768,
	.vsync_start = 768 + 16,
	.vsync_end = 768 + 16 + 8,
	.vtotal = 768 + 16 + 8 + 16,
	.vrefresh = 60,
};

static const struct panel_desc chunghwa_claa101wb01 = {
	.modes = &chunghwa_claa101wb01_mode,
	.num_modes = 1,
	.bpc = 6,
	.size = {
		.width = 223,
		.height = 125,
	},
};

static const struct drm_display_mode edt_et057090dhu_mode = {
	.clock = 25175,
	.hdisplay = 640,
	.hsync_start = 640 + 16,
	.hsync_end = 640 + 16 + 30,
	.htotal = 640 + 16 + 30 + 114,
	.vdisplay = 480,
	.vsync_start = 480 + 10,
	.vsync_end = 480 + 10 + 3,
	.vtotal = 480 + 10 + 3 + 32,
	.vrefresh = 60,
	.flags = DRM_MODE_FLAG_NVSYNC | DRM_MODE_FLAG_NHSYNC,
};

static const struct panel_desc edt_et057090dhu = {
	.modes = &edt_et057090dhu_mode,
	.num_modes = 1,
	.bpc = 6,
	.size = {
		.width = 115,
		.height = 86,
	},
	.bus_format = MEDIA_BUS_FMT_RGB666_1X18,
	.bus_flags = DRM_BUS_FLAG_DE_HIGH | DRM_BUS_FLAG_PIXDATA_NEGEDGE,
};

static const struct drm_display_mode edt_etm0700g0dh6_mode = {
	.clock = 33260,
	.hdisplay = 800,
	.hsync_start = 800 + 40,
	.hsync_end = 800 + 40 + 128,
	.htotal = 800 + 40 + 128 + 88,
	.vdisplay = 480,
	.vsync_start = 480 + 10,
	.vsync_end = 480 + 10 + 2,
	.vtotal = 480 + 10 + 2 + 33,
	.vrefresh = 60,
	.flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC,
};

static const struct panel_desc edt_etm0700g0dh6 = {
	.modes = &edt_etm0700g0dh6_mode,
	.num_modes = 1,
	.bpc = 6,
	.size = {
		.width = 152,
		.height = 91,
	},
	.bus_format = MEDIA_BUS_FMT_RGB666_1X18,
	.bus_flags = DRM_BUS_FLAG_DE_HIGH | DRM_BUS_FLAG_PIXDATA_NEGEDGE,
};

static const struct drm_display_mode foxlink_fl500wvr00_a0t_mode = {
	.clock = 32260,
	.hdisplay = 800,
	.hsync_start = 800 + 168,
	.hsync_end = 800 + 168 + 64,
	.htotal = 800 + 168 + 64 + 88,
	.vdisplay = 480,
	.vsync_start = 480 + 37,
	.vsync_end = 480 + 37 + 2,
	.vtotal = 480 + 37 + 2 + 8,
	.vrefresh = 60,
};

static const struct panel_desc foxlink_fl500wvr00_a0t = {
	.modes = &foxlink_fl500wvr00_a0t_mode,
	.num_modes = 1,
	.bpc = 8,
	.size = {
		.width = 108,
		.height = 65,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X24,
};

static const struct drm_display_mode giantplus_gpg482739qs5_mode = {
	.clock = 9000,
	.hdisplay = 480,
	.hsync_start = 480 + 5,
	.hsync_end = 480 + 5 + 1,
	.htotal = 480 + 5 + 1 + 40,
	.vdisplay = 272,
	.vsync_start = 272 + 8,
	.vsync_end = 272 + 8 + 1,
	.vtotal = 272 + 8 + 1 + 8,
	.vrefresh = 60,
};

static const struct panel_desc giantplus_gpg482739qs5 = {
	.modes = &giantplus_gpg482739qs5_mode,
	.num_modes = 1,
	.bpc = 8,
	.size = {
		.width = 95,
		.height = 54,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X24,
};

static const struct display_timing hannstar_hsd070pww1_timing = {
	.pixelclock = { 64300000, 71100000, 82000000 },
	.hactive = { 1280, 1280, 1280 },
	.hfront_porch = { 1, 1, 10 },
	.hback_porch = { 1, 1, 10 },
	/*
	 * According to the data sheet, the minimum horizontal blanking interval
	 * is 54 clocks (1 + 52 + 1), but tests with a Nitrogen6X have shown the
	 * minimum working horizontal blanking interval to be 60 clocks.
	 */
	.hsync_len = { 58, 158, 661 },
	.vactive = { 800, 800, 800 },
	.vfront_porch = { 1, 1, 10 },
	.vback_porch = { 1, 1, 10 },
	.vsync_len = { 1, 21, 203 },
	.flags = DISPLAY_FLAGS_DE_HIGH,
};

static const struct panel_desc hannstar_hsd070pww1 = {
	.timings = &hannstar_hsd070pww1_timing,
	.num_timings = 1,
	.bpc = 6,
	.size = {
		.width = 151,
		.height = 94,
	},
	.bus_format = MEDIA_BUS_FMT_RGB666_1X7X3_SPWG,
};

static const struct display_timing hannstar_hsd100pxn1_timing = {
	.pixelclock = { 55000000, 65000000, 75000000 },
	.hactive = { 1024, 1024, 1024 },
	.hfront_porch = { 40, 40, 40 },
	.hback_porch = { 220, 220, 220 },
	.hsync_len = { 20, 60, 100 },
	.vactive = { 768, 768, 768 },
	.vfront_porch = { 7, 7, 7 },
	.vback_porch = { 21, 21, 21 },
	.vsync_len = { 10, 10, 10 },
	.flags = DISPLAY_FLAGS_DE_HIGH,
};

static const struct panel_desc hannstar_hsd100pxn1 = {
	.timings = &hannstar_hsd100pxn1_timing,
	.num_timings = 1,
	.bpc = 6,
	.size = {
		.width = 203,
		.height = 152,
	},
	.bus_format = MEDIA_BUS_FMT_RGB666_1X7X3_SPWG,
};

static const struct drm_display_mode hitachi_tx23d38vm0caa_mode = {
	.clock = 33333,
	.hdisplay = 800,
	.hsync_start = 800 + 85,
	.hsync_end = 800 + 85 + 86,
	.htotal = 800 + 85 + 86 + 85,
	.vdisplay = 480,
	.vsync_start = 480 + 16,
	.vsync_end = 480 + 16 + 13,
	.vtotal = 480 + 16 + 13 + 16,
	.vrefresh = 60,
};

static const struct panel_desc hitachi_tx23d38vm0caa = {
	.modes = &hitachi_tx23d38vm0caa_mode,
	.num_modes = 1,
	.bpc = 6,
	.size = {
		.width = 195,
		.height = 117,
	},
};

static const struct drm_display_mode innolux_at043tn24_mode = {
	.clock = 9000,
	.hdisplay = 480,
	.hsync_start = 480 + 2,
	.hsync_end = 480 + 2 + 41,
	.htotal = 480 + 2 + 41 + 2,
	.vdisplay = 272,
	.vsync_start = 272 + 2,
	.vsync_end = 272 + 2 + 11,
	.vtotal = 272 + 2 + 11 + 2,
	.vrefresh = 60,
	.flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC,
};

static const struct panel_desc innolux_at043tn24 = {
	.modes = &innolux_at043tn24_mode,
	.num_modes = 1,
	.bpc = 8,
	.size = {
		.width = 95,
		.height = 54,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X24,
};

static const struct drm_display_mode innolux_at070tn92_mode = {
	.clock = 33333,
	.hdisplay = 800,
	.hsync_start = 800 + 210,
	.hsync_end = 800 + 210 + 20,
	.htotal = 800 + 210 + 20 + 46,
	.vdisplay = 480,
	.vsync_start = 480 + 22,
	.vsync_end = 480 + 22 + 10,
	.vtotal = 480 + 22 + 23 + 10,
	.vrefresh = 60,
};

static const struct panel_desc innolux_at070tn92 = {
	.modes = &innolux_at070tn92_mode,
	.num_modes = 1,
	.size = {
		.width = 154,
		.height = 86,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X24,
};

static const struct display_timing innolux_g101ice_l01_timing = {
	.pixelclock = { 60400000, 71100000, 74700000 },
	.hactive = { 1280, 1280, 1280 },
	.hfront_porch = { 41, 80, 100 },
	.hback_porch = { 40, 79, 99 },
	.hsync_len = { 1, 1, 1 },
	.vactive = { 800, 800, 800 },
	.vfront_porch = { 5, 11, 14 },
	.vback_porch = { 4, 11, 14 },
	.vsync_len = { 1, 1, 1 },
	.flags = DISPLAY_FLAGS_DE_HIGH,
};

static const struct panel_desc innolux_g101ice_l01 = {
	.timings = &innolux_g101ice_l01_timing,
	.num_timings = 1,
	.bpc = 8,
	.size = {
		.width = 217,
		.height = 135,
	},
	.delay = {
		.enable = 200,
		.disable = 200,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X7X4_SPWG,
};

static const struct display_timing innolux_g121i1_l01_timing = {
	.pixelclock = { 67450000, 71000000, 74550000 },
	.hactive = { 1280, 1280, 1280 },
	.hfront_porch = { 40, 80, 160 },
	.hback_porch = { 39, 79, 159 },
	.hsync_len = { 1, 1, 1 },
	.vactive = { 800, 800, 800 },
	.vfront_porch = { 5, 11, 100 },
	.vback_porch = { 4, 11, 99 },
	.vsync_len = { 1, 1, 1 },
};

static const struct panel_desc innolux_g121i1_l01 = {
	.timings = &innolux_g121i1_l01_timing,
	.num_timings = 1,
	.bpc = 6,
	.size = {
		.width = 261,
		.height = 163,
	},
	.delay = {
		.enable = 200,
		.disable = 20,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X7X4_SPWG,
};

static const struct drm_display_mode innolux_g121x1_l03_mode = {
	.clock = 65000,
	.hdisplay = 1024,
	.hsync_start = 1024 + 0,
	.hsync_end = 1024 + 1,
	.htotal = 1024 + 0 + 1 + 320,
	.vdisplay = 768,
	.vsync_start = 768 + 38,
	.vsync_end = 768 + 38 + 1,
	.vtotal = 768 + 38 + 1 + 0,
	.vrefresh = 60,
	.flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC,
};

static const struct panel_desc innolux_g121x1_l03 = {
	.modes = &innolux_g121x1_l03_mode,
	.num_modes = 1,
	.bpc = 6,
	.size = {
		.width = 246,
		.height = 185,
	},
	.delay = {
		.enable = 200,
		.unprepare = 200,
		.disable = 400,
	},
};

static const struct drm_display_mode innolux_n116bge_mode = {
	.clock = 76420,
	.hdisplay = 1366,
	.hsync_start = 1366 + 136,
	.hsync_end = 1366 + 136 + 30,
	.htotal = 1366 + 136 + 30 + 60,
	.vdisplay = 768,
	.vsync_start = 768 + 8,
	.vsync_end = 768 + 8 + 12,
	.vtotal = 768 + 8 + 12 + 12,
	.vrefresh = 60,
	.flags = DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC,
};

static const struct panel_desc innolux_n116bge = {
	.modes = &innolux_n116bge_mode,
	.num_modes = 1,
	.bpc = 6,
	.size = {
		.width = 256,
		.height = 144,
	},
};

static const struct drm_display_mode innolux_n156bge_l21_mode = {
	.clock = 69300,
	.hdisplay = 1366,
	.hsync_start = 1366 + 16,
	.hsync_end = 1366 + 16 + 34,
	.htotal = 1366 + 16 + 34 + 50,
	.vdisplay = 768,
	.vsync_start = 768 + 2,
	.vsync_end = 768 + 2 + 6,
	.vtotal = 768 + 2 + 6 + 12,
	.vrefresh = 60,
};

static const struct panel_desc innolux_n156bge_l21 = {
	.modes = &innolux_n156bge_l21_mode,
	.num_modes = 1,
	.bpc = 6,
	.size = {
		.width = 344,
		.height = 193,
	},
};

static const struct drm_display_mode innolux_zj070na_01p_mode = {
	.clock = 51501,
	.hdisplay = 1024,
	.hsync_start = 1024 + 128,
	.hsync_end = 1024 + 128 + 64,
	.htotal = 1024 + 128 + 64 + 128,
	.vdisplay = 600,
	.vsync_start = 600 + 16,
	.vsync_end = 600 + 16 + 4,
	.vtotal = 600 + 16 + 4 + 16,
	.vrefresh = 60,
};

static const struct panel_desc innolux_zj070na_01p = {
	.modes = &innolux_zj070na_01p_mode,
	.num_modes = 1,
	.bpc = 6,
	.size = {
		.width = 154,
		.height = 90,
	},
};

static const struct display_timing jdi_tx26d202vm0bwa_timing = {
	.pixelclock = { 151820000, 156720000, 159780000 },
	.hactive = { 1920, 1920, 1920 },
	.hfront_porch = { 76, 100, 112 },
	.hback_porch = { 74, 100, 112 },
	.hsync_len = { 30, 30, 30 },
	.vactive = { 1200, 1200, 1200},
	.vfront_porch = { 3, 5, 10 },
	.vback_porch = { 2, 5, 10 },
	.vsync_len = { 5, 5, 5 },
	.flags = DISPLAY_FLAGS_DE_HIGH,
};

static const struct panel_desc jdi_tx26d202vm0bwa = {
	.timings = &jdi_tx26d202vm0bwa_timing,
	.num_timings = 1,
	.bpc = 8,
	.size = {
		.width = 217,
		.height = 136,
	},
	.delay = {
		/*
		 * The panel spec recommends one second delay
		 * to the below items.  However, it's a bit too
		 * long in practice.  Based on tests, it turns
		 * out 100 milliseconds is fine.
		 */
		.prepare = 100,
		.enable = 100,
		.unprepare = 100,
		.disable = 100,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X7X4_SPWG,
};

static const struct display_timing kyo_tcg121xglp_timing = {
	.pixelclock = { 52000000, 65000000, 71000000 },
	.hactive = { 1024, 1024, 1024 },
	.hfront_porch = { 2, 2, 2 },
	.hback_porch = { 2, 2, 2 },
	.hsync_len = { 86, 124, 244 },
	.vactive = { 768, 768, 768 },
	.vfront_porch = { 2, 2, 2 },
	.vback_porch = { 2, 2, 2 },
	.vsync_len = { 6, 34, 73 },
	.flags = DISPLAY_FLAGS_DE_HIGH,
};

static const struct panel_desc kyo_tcg121xglp = {
	.timings = &kyo_tcg121xglp_timing,
	.num_timings = 1,
	.bpc = 8,
	.size = {
		.width = 246,
		.height = 184,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X7X4_SPWG,
};

static const struct drm_display_mode lg_lb070wv8_mode = {
	.clock = 33246,
	.hdisplay = 800,
	.hsync_start = 800 + 88,
	.hsync_end = 800 + 88 + 80,
	.htotal = 800 + 88 + 80 + 88,
	.vdisplay = 480,
	.vsync_start = 480 + 10,
	.vsync_end = 480 + 10 + 25,
	.vtotal = 480 + 10 + 25 + 10,
	.vrefresh = 60,
};

static const struct panel_desc lg_lb070wv8 = {
	.modes = &lg_lb070wv8_mode,
	.num_modes = 1,
	.bpc = 16,
	.size = {
		.width = 151,
		.height = 91,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X7X4_SPWG,
};

static const struct drm_display_mode lg_lp079qx1_sp0v_mode = {
	.clock = 200000,
	.hdisplay = 1536,
	.hsync_start = 1536 + 12,
	.hsync_end = 1536 + 12 + 16,
	.htotal = 1536 + 12 + 16 + 48,
	.vdisplay = 2048,
	.vsync_start = 2048 + 8,
	.vsync_end = 2048 + 8 + 4,
	.vtotal = 2048 + 8 + 4 + 8,
	.vrefresh = 60,
	.flags = DRM_MODE_FLAG_NVSYNC | DRM_MODE_FLAG_NHSYNC,
};

static const struct panel_desc lg_lp079qx1_sp0v = {
	.modes = &lg_lp079qx1_sp0v_mode,
	.num_modes = 1,
	.size = {
		.width = 129,
		.height = 171,
	},
};

static const struct drm_display_mode lg_lp097qx1_spa1_mode = {
	.clock = 205210,
	.hdisplay = 2048,
	.hsync_start = 2048 + 150,
	.hsync_end = 2048 + 150 + 5,
	.htotal = 2048 + 150 + 5 + 5,
	.vdisplay = 1536,
	.vsync_start = 1536 + 3,
	.vsync_end = 1536 + 3 + 1,
	.vtotal = 1536 + 3 + 1 + 9,
	.vrefresh = 60,
};

static const struct panel_desc lg_lp097qx1_spa1 = {
	.modes = &lg_lp097qx1_spa1_mode,
	.num_modes = 1,
	.size = {
		.width = 208,
		.height = 147,
	},
};

static const struct drm_display_mode lg_lp120up1_mode = {
	.clock = 162300,
	.hdisplay = 1920,
	.hsync_start = 1920 + 40,
	.hsync_end = 1920 + 40 + 40,
	.htotal = 1920 + 40 + 40+ 80,
	.vdisplay = 1280,
	.vsync_start = 1280 + 4,
	.vsync_end = 1280 + 4 + 4,
	.vtotal = 1280 + 4 + 4 + 12,
	.vrefresh = 60,
};

static const struct panel_desc lg_lp120up1 = {
	.modes = &lg_lp120up1_mode,
	.num_modes = 1,
	.bpc = 8,
	.size = {
		.width = 267,
		.height = 183,
	},
};

static const struct drm_display_mode lg_lp129qe_mode = {
	.clock = 285250,
	.hdisplay = 2560,
	.hsync_start = 2560 + 48,
	.hsync_end = 2560 + 48 + 32,
	.htotal = 2560 + 48 + 32 + 80,
	.vdisplay = 1700,
	.vsync_start = 1700 + 3,
	.vsync_end = 1700 + 3 + 10,
	.vtotal = 1700 + 3 + 10 + 36,
	.vrefresh = 60,
};

static const struct panel_desc lg_lp129qe = {
	.modes = &lg_lp129qe_mode,
	.num_modes = 1,
	.bpc = 8,
	.size = {
		.width = 272,
		.height = 181,
	},
};

static const struct display_timing nec_nl12880bc20_05_timing = {
	.pixelclock = { 67000000, 71000000, 75000000 },
	.hactive = { 1280, 1280, 1280 },
	.hfront_porch = { 2, 30, 30 },
	.hback_porch = { 6, 100, 100 },
	.hsync_len = { 2, 30, 30 },
	.vactive = { 800, 800, 800 },
	.vfront_porch = { 5, 5, 5 },
	.vback_porch = { 11, 11, 11 },
	.vsync_len = { 7, 7, 7 },
};

static const struct panel_desc nec_nl12880bc20_05 = {
	.timings = &nec_nl12880bc20_05_timing,
	.num_timings = 1,
	.bpc = 8,
	.size = {
		.width = 261,
		.height = 163,
	},
	.delay = {
		.enable = 50,
		.disable = 50,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X7X4_SPWG,
};

static const struct drm_display_mode nec_nl4827hc19_05b_mode = {
	.clock = 10870,
	.hdisplay = 480,
	.hsync_start = 480 + 2,
	.hsync_end = 480 + 2 + 41,
	.htotal = 480 + 2 + 41 + 2,
	.vdisplay = 272,
	.vsync_start = 272 + 2,
	.vsync_end = 272 + 2 + 4,
	.vtotal = 272 + 2 + 4 + 2,
	.vrefresh = 74,
	.flags = DRM_MODE_FLAG_NVSYNC | DRM_MODE_FLAG_NHSYNC,
};

static const struct panel_desc nec_nl4827hc19_05b = {
	.modes = &nec_nl4827hc19_05b_mode,
	.num_modes = 1,
	.bpc = 8,
	.size = {
		.width = 95,
		.height = 54,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X24,
	.bus_flags = DRM_BUS_FLAG_PIXDATA_POSEDGE,
};

static const struct drm_display_mode netron_dy_e231732_mode = {
	.clock = 66000,
	.hdisplay = 1024,
	.hsync_start = 1024 + 160,
	.hsync_end = 1024 + 160 + 70,
	.htotal = 1024 + 160 + 70 + 90,
	.vdisplay = 600,
	.vsync_start = 600 + 127,
	.vsync_end = 600 + 127 + 20,
	.vtotal = 600 + 127 + 20 + 3,
	.vrefresh = 60,
};

static const struct panel_desc netron_dy_e231732 = {
	.modes = &netron_dy_e231732_mode,
	.num_modes = 1,
	.size = {
		.width = 154,
		.height = 87,
	},
	.bus_format = MEDIA_BUS_FMT_RGB666_1X18,
};

static const struct display_timing nlt_nl192108ac18_02d_timing = {
	.pixelclock = { 130000000, 148350000, 163000000 },
	.hactive = { 1920, 1920, 1920 },
	.hfront_porch = { 80, 100, 100 },
	.hback_porch = { 100, 120, 120 },
	.hsync_len = { 50, 60, 60 },
	.vactive = { 1080, 1080, 1080 },
	.vfront_porch = { 12, 30, 30 },
	.vback_porch = { 4, 10, 10 },
	.vsync_len = { 4, 5, 5 },
};

static const struct panel_desc nlt_nl192108ac18_02d = {
	.timings = &nlt_nl192108ac18_02d_timing,
	.num_timings = 1,
	.bpc = 8,
	.size = {
		.width = 344,
		.height = 194,
	},
	.delay = {
		.unprepare = 500,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X7X4_SPWG,
};

static const struct drm_display_mode nvd_9128_mode = {
	.clock = 29500,
	.hdisplay = 800,
	.hsync_start = 800 + 130,
	.hsync_end = 800 + 130 + 98,
	.htotal = 800 + 0 + 130 + 98,
	.vdisplay = 480,
	.vsync_start = 480 + 10,
	.vsync_end = 480 + 10 + 50,
	.vtotal = 480 + 0 + 10 + 50,
};

static const struct panel_desc nvd_9128 = {
	.modes = &nvd_9128_mode,
	.num_modes = 1,
	.bpc = 8,
	.size = {
		.width = 156,
		.height = 88,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X7X4_SPWG,
};

static const struct display_timing okaya_rs800480t_7x0gp_timing = {
	.pixelclock = { 30000000, 30000000, 40000000 },
	.hactive = { 800, 800, 800 },
	.hfront_porch = { 40, 40, 40 },
	.hback_porch = { 40, 40, 40 },
	.hsync_len = { 1, 48, 48 },
	.vactive = { 480, 480, 480 },
	.vfront_porch = { 13, 13, 13 },
	.vback_porch = { 29, 29, 29 },
	.vsync_len = { 3, 3, 3 },
	.flags = DISPLAY_FLAGS_DE_HIGH,
};

static const struct panel_desc okaya_rs800480t_7x0gp = {
	.timings = &okaya_rs800480t_7x0gp_timing,
	.num_timings = 1,
	.bpc = 6,
	.size = {
		.width = 154,
		.height = 87,
	},
	.delay = {
		.prepare = 41,
		.enable = 50,
		.unprepare = 41,
		.disable = 50,
	},
	.bus_format = MEDIA_BUS_FMT_RGB666_1X18,
};

static const struct drm_display_mode olimex_lcd_olinuxino_43ts_mode = {
	.clock = 9000,
	.hdisplay = 480,
	.hsync_start = 480 + 5,
	.hsync_end = 480 + 5 + 30,
	.htotal = 480 + 5 + 30 + 10,
	.vdisplay = 272,
	.vsync_start = 272 + 8,
	.vsync_end = 272 + 8 + 5,
	.vtotal = 272 + 8 + 5 + 3,
	.vrefresh = 60,
};

static const struct panel_desc olimex_lcd_olinuxino_43ts = {
	.modes = &olimex_lcd_olinuxino_43ts_mode,
	.num_modes = 1,
	.size = {
		.width = 105,
		.height = 67,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X24,
};

/*
 * 800x480 CVT. The panel appears to be quite accepting, at least as far as
 * pixel clocks, but this is the timing that was being used in the Adafruit
 * installation instructions.
 */
static const struct drm_display_mode ontat_yx700wv03_mode = {
	.clock = 29500,
	.hdisplay = 800,
	.hsync_start = 824,
	.hsync_end = 896,
	.htotal = 992,
	.vdisplay = 480,
	.vsync_start = 483,
	.vsync_end = 493,
	.vtotal = 500,
	.vrefresh = 60,
	.flags = DRM_MODE_FLAG_NVSYNC | DRM_MODE_FLAG_NHSYNC,
};

/*
 * Specification at:
 * https://www.adafruit.com/images/product-files/2406/c3163.pdf
 */
static const struct panel_desc ontat_yx700wv03 = {
	.modes = &ontat_yx700wv03_mode,
	.num_modes = 1,
	.bpc = 8,
	.size = {
		.width = 154,
		.height = 83,
	},
	.bus_format = MEDIA_BUS_FMT_RGB666_1X18,
};

static const struct drm_display_mode ortustech_com43h4m85ulc_mode  = {
	.clock = 25000,
	.hdisplay = 480,
	.hsync_start = 480 + 10,
	.hsync_end = 480 + 10 + 10,
	.htotal = 480 + 10 + 10 + 15,
	.vdisplay = 800,
	.vsync_start = 800 + 3,
	.vsync_end = 800 + 3 + 3,
	.vtotal = 800 + 3 + 3 + 3,
	.vrefresh = 60,
};

static const struct panel_desc ortustech_com43h4m85ulc = {
	.modes = &ortustech_com43h4m85ulc_mode,
	.num_modes = 1,
	.bpc = 8,
	.size = {
		.width = 56,
		.height = 93,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X24,
	.bus_flags = DRM_BUS_FLAG_DE_HIGH | DRM_BUS_FLAG_PIXDATA_POSEDGE,
};

static const struct drm_display_mode qd43003c0_40_mode = {
	.clock = 9000,
	.hdisplay = 480,
	.hsync_start = 480 + 8,
	.hsync_end = 480 + 8 + 4,
	.htotal = 480 + 8 + 4 + 39,
	.vdisplay = 272,
	.vsync_start = 272 + 4,
	.vsync_end = 272 + 4 + 10,
	.vtotal = 272 + 4 + 10 + 2,
	.vrefresh = 60,
};

static const struct panel_desc qd43003c0_40 = {
	.modes = &qd43003c0_40_mode,
	.num_modes = 1,
	.bpc = 8,
	.size = {
		.width = 95,
		.height = 53,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X24,
};

static const struct drm_display_mode samsung_lsn122dl01_c01_mode = {
	.clock = 271560,
	.hdisplay = 2560,
	.hsync_start = 2560 + 48,
	.hsync_end = 2560 + 48 + 32,
	.htotal = 2560 + 48 + 32 + 80,
	.vdisplay = 1600,
	.vsync_start = 1600 + 2,
	.vsync_end = 1600 + 2 + 5,
	.vtotal = 1600 + 2 + 5 + 57,
	.vrefresh = 60,
};

static const struct panel_desc samsung_lsn122dl01_c01 = {
	.modes = &samsung_lsn122dl01_c01_mode,
	.num_modes = 1,
	.size = {
		.width = 263,
		.height = 164,
	},
};

static const struct drm_display_mode samsung_ltn101nt05_mode = {
	.clock = 54030,
	.hdisplay = 1024,
	.hsync_start = 1024 + 24,
	.hsync_end = 1024 + 24 + 136,
	.htotal = 1024 + 24 + 136 + 160,
	.vdisplay = 600,
	.vsync_start = 600 + 3,
	.vsync_end = 600 + 3 + 6,
	.vtotal = 600 + 3 + 6 + 61,
	.vrefresh = 60,
};

static const struct panel_desc samsung_ltn101nt05 = {
	.modes = &samsung_ltn101nt05_mode,
	.num_modes = 1,
	.bpc = 6,
	.size = {
		.width = 223,
		.height = 125,
	},
};

static const struct drm_display_mode samsung_ltn140at29_301_mode = {
	.clock = 76300,
	.hdisplay = 1366,
	.hsync_start = 1366 + 64,
	.hsync_end = 1366 + 64 + 48,
	.htotal = 1366 + 64 + 48 + 128,
	.vdisplay = 768,
	.vsync_start = 768 + 2,
	.vsync_end = 768 + 2 + 5,
	.vtotal = 768 + 2 + 5 + 17,
	.vrefresh = 60,
};

static const struct panel_desc samsung_ltn140at29_301 = {
	.modes = &samsung_ltn140at29_301_mode,
	.num_modes = 1,
	.bpc = 6,
	.size = {
		.width = 320,
		.height = 187,
	},
};

static const struct display_timing sharp_lq101k1ly04_timing = {
	.pixelclock = { 60000000, 65000000, 80000000 },
	.hactive = { 1280, 1280, 1280 },
	.hfront_porch = { 20, 20, 20 },
	.hback_porch = { 20, 20, 20 },
	.hsync_len = { 10, 10, 10 },
	.vactive = { 800, 800, 800 },
	.vfront_porch = { 4, 4, 4 },
	.vback_porch = { 4, 4, 4 },
	.vsync_len = { 4, 4, 4 },
	.flags = DISPLAY_FLAGS_PIXDATA_POSEDGE,
};

static const struct panel_desc sharp_lq101k1ly04 = {
	.timings = &sharp_lq101k1ly04_timing,
	.num_timings = 1,
	.bpc = 8,
	.size = {
		.width = 217,
		.height = 136,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X7X4_JEIDA,
};

static const struct drm_display_mode sharp_lq123p1jx31_mode = {
	.clock = 252750,
	.hdisplay = 2400,
	.hsync_start = 2400 + 48,
	.hsync_end = 2400 + 48 + 32,
	.htotal = 2400 + 48 + 32 + 80,
	.vdisplay = 1600,
	.vsync_start = 1600 + 3,
	.vsync_end = 1600 + 3 + 10,
	.vtotal = 1600 + 3 + 10 + 33,
	.vrefresh = 60,
	.flags = DRM_MODE_FLAG_NVSYNC | DRM_MODE_FLAG_NHSYNC,
};

static const struct panel_desc sharp_lq123p1jx31 = {
	.modes = &sharp_lq123p1jx31_mode,
	.num_modes = 1,
	.bpc = 8,
	.size = {
		.width = 259,
		.height = 173,
	},
	.delay = {
		.prepare = 110,
		.enable = 50,
		.unprepare = 550,
	},
};

static const struct drm_display_mode sharp_lq150x1lg11_mode = {
	.clock = 71100,
	.hdisplay = 1024,
	.hsync_start = 1024 + 168,
	.hsync_end = 1024 + 168 + 64,
	.htotal = 1024 + 168 + 64 + 88,
	.vdisplay = 768,
	.vsync_start = 768 + 37,
	.vsync_end = 768 + 37 + 2,
	.vtotal = 768 + 37 + 2 + 8,
	.vrefresh = 60,
};

static const struct panel_desc sharp_lq150x1lg11 = {
	.modes = &sharp_lq150x1lg11_mode,
	.num_modes = 1,
	.bpc = 6,
	.size = {
		.width = 304,
		.height = 228,
	},
	.bus_format = MEDIA_BUS_FMT_RGB565_1X16,
};

static const struct drm_display_mode shelly_sca07010_bfn_lnn_mode = {
	.clock = 33300,
	.hdisplay = 800,
	.hsync_start = 800 + 1,
	.hsync_end = 800 + 1 + 64,
	.htotal = 800 + 1 + 64 + 64,
	.vdisplay = 480,
	.vsync_start = 480 + 1,
	.vsync_end = 480 + 1 + 23,
	.vtotal = 480 + 1 + 23 + 22,
	.vrefresh = 60,
};

static const struct panel_desc shelly_sca07010_bfn_lnn = {
	.modes = &shelly_sca07010_bfn_lnn_mode,
	.num_modes = 1,
	.size = {
		.width = 152,
		.height = 91,
	},
	.bus_format = MEDIA_BUS_FMT_RGB666_1X18,
};

static const struct drm_display_mode starry_kr122ea0sra_mode = {
	.clock = 147000,
	.hdisplay = 1920,
	.hsync_start = 1920 + 16,
	.hsync_end = 1920 + 16 + 16,
	.htotal = 1920 + 16 + 16 + 32,
	.vdisplay = 1200,
	.vsync_start = 1200 + 15,
	.vsync_end = 1200 + 15 + 2,
	.vtotal = 1200 + 15 + 2 + 18,
	.vrefresh = 60,
	.flags = DRM_MODE_FLAG_NVSYNC | DRM_MODE_FLAG_NHSYNC,
};

static const struct panel_desc starry_kr122ea0sra = {
	.modes = &starry_kr122ea0sra_mode,
	.num_modes = 1,
	.size = {
		.width = 263,
		.height = 164,
	},
	.delay = {
		.prepare = 10 + 200,
		.enable = 50,
		.unprepare = 10 + 500,
	},
};

static const struct display_timing tianma_tm070jdhg30_timing = {
	.pixelclock = { 62600000, 68200000, 78100000 },
	.hactive = { 1280, 1280, 1280 },
	.hfront_porch = { 15, 64, 159 },
	.hback_porch = { 5, 5, 5 },
	.hsync_len = { 1, 1, 256 },
	.vactive = { 800, 800, 800 },
	.vfront_porch = { 3, 40, 99 },
	.vback_porch = { 2, 2, 2 },
	.vsync_len = { 1, 1, 128 },
	.flags = DISPLAY_FLAGS_DE_HIGH,
};

static const struct panel_desc tianma_tm070jdhg30 = {
	.timings = &tianma_tm070jdhg30_timing,
	.num_timings = 1,
	.bpc = 8,
	.size = {
		.width = 151,
		.height = 95,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X7X4_SPWG,
};

static const struct drm_display_mode tpk_f07a_0102_mode = {
	.clock = 33260,
	.hdisplay = 800,
	.hsync_start = 800 + 40,
	.hsync_end = 800 + 40 + 128,
	.htotal = 800 + 40 + 128 + 88,
	.vdisplay = 480,
	.vsync_start = 480 + 10,
	.vsync_end = 480 + 10 + 2,
	.vtotal = 480 + 10 + 2 + 33,
	.vrefresh = 60,
};

static const struct panel_desc tpk_f07a_0102 = {
	.modes = &tpk_f07a_0102_mode,
	.num_modes = 1,
	.size = {
		.width = 152,
		.height = 91,
	},
	.bus_flags = DRM_BUS_FLAG_PIXDATA_POSEDGE,
};

static const struct drm_display_mode tpk_f10a_0102_mode = {
	.clock = 45000,
	.hdisplay = 1024,
	.hsync_start = 1024 + 176,
	.hsync_end = 1024 + 176 + 5,
	.htotal = 1024 + 176 + 5 + 88,
	.vdisplay = 600,
	.vsync_start = 600 + 20,
	.vsync_end = 600 + 20 + 5,
	.vtotal = 600 + 20 + 5 + 25,
	.vrefresh = 60,
};

static const struct panel_desc tpk_f10a_0102 = {
	.modes = &tpk_f10a_0102_mode,
	.num_modes = 1,
	.size = {
		.width = 223,
		.height = 125,
	},
};

static const struct display_timing urt_umsh_8596md_timing = {
	.pixelclock = { 33260000, 33260000, 33260000 },
	.hactive = { 800, 800, 800 },
	.hfront_porch = { 41, 41, 41 },
	.hback_porch = { 216 - 128, 216 - 128, 216 - 128 },
	.hsync_len = { 71, 128, 128 },
	.vactive = { 480, 480, 480 },
	.vfront_porch = { 10, 10, 10 },
	.vback_porch = { 35 - 2, 35 - 2, 35 - 2 },
	.vsync_len = { 2, 2, 2 },
	.flags = DISPLAY_FLAGS_DE_HIGH | DISPLAY_FLAGS_PIXDATA_NEGEDGE |
		DISPLAY_FLAGS_HSYNC_LOW | DISPLAY_FLAGS_VSYNC_LOW,
};

static const struct panel_desc urt_umsh_8596md_lvds = {
	.timings = &urt_umsh_8596md_timing,
	.num_timings = 1,
	.bpc = 6,
	.size = {
		.width = 152,
		.height = 91,
	},
	.bus_format = MEDIA_BUS_FMT_RGB666_1X7X3_SPWG,
};

static const struct panel_desc urt_umsh_8596md_parallel = {
	.timings = &urt_umsh_8596md_timing,
	.num_timings = 1,
	.bpc = 6,
	.size = {
		.width = 152,
		.height = 91,
	},
	.bus_format = MEDIA_BUS_FMT_RGB666_1X18,
};

static const struct drm_display_mode winstar_wf35ltiacd_mode = {
	.clock = 6410,
	.hdisplay = 320,
	.hsync_start = 320 + 20,
	.hsync_end = 320 + 20 + 30,
	.htotal = 320 + 20 + 30 + 38,
	.vdisplay = 240,
	.vsync_start = 240 + 4,
	.vsync_end = 240 + 4 + 3,
	.vtotal = 240 + 4 + 3 + 15,
	.vrefresh = 60,
	.flags = DRM_MODE_FLAG_NVSYNC | DRM_MODE_FLAG_NHSYNC,
};

static const struct panel_desc winstar_wf35ltiacd = {
	.modes = &winstar_wf35ltiacd_mode,
	.num_modes = 1,
	.bpc = 8,
	.size = {
		.width = 70,
		.height = 53,
	},
	.bus_format = MEDIA_BUS_FMT_RGB888_1X24,
};

static const struct of_device_id platform_of_match[] = {
	{
		.compatible = "ampire,am-480272h3tmqw-t01h",
		.data = &ampire_am_480272h3tmqw_t01h,
	}, {
		.compatible = "ampire,am800480r3tmqwa1h",
		.data = &ampire_am800480r3tmqwa1h,
	}, {
		.compatible = "auo,b101aw03",
		.data = &auo_b101aw03,
	}, {
		.compatible = "auo,b101ean01",
		.data = &auo_b101ean01,
	}, {
		.compatible = "auo,b101xtn01",
		.data = &auo_b101xtn01,
	}, {
		.compatible = "auo,b116xw03",
		.data = &auo_b116xw03,
	}, {
		.compatible = "auo,b133htn01",
		.data = &auo_b133htn01,
	}, {
		.compatible = "auo,b133xtn01",
		.data = &auo_b133xtn01,
	}, {
		.compatible = "auo,g133han01",
		.data = &auo_g133han01,
	}, {
		.compatible = "auo,g185han01",
		.data = &auo_g185han01,
	}, {
		.compatible = "auo,p320hvn03",
		.data = &auo_p320hvn03,
	}, {
		.compatible = "auo,t215hvn01",
		.data = &auo_t215hvn01,
	}, {
		.compatible = "avic,tm070ddh03",
		.data = &avic_tm070ddh03,
	}, {
		.compatible = "boe,nv101wxmn51",
		.data = &boe_nv101wxmn51,
	}, {
		.compatible = "chunghwa,claa070wp03xg",
		.data = &chunghwa_claa070wp03xg,
	}, {
		.compatible = "chunghwa,claa101wa01a",
		.data = &chunghwa_claa101wa01a
	}, {
		.compatible = "chunghwa,claa101wb01",
		.data = &chunghwa_claa101wb01
	}, {
		.compatible = "edt,et057090dhu",
		.data = &edt_et057090dhu,
	}, {
		.compatible = "edt,et070080dh6",
		.data = &edt_etm0700g0dh6,
	}, {
		.compatible = "edt,etm0700g0dh6",
		.data = &edt_etm0700g0dh6,
	}, {
		.compatible = "foxlink,fl500wvr00-a0t",
		.data = &foxlink_fl500wvr00_a0t,
	}, {
		.compatible = "giantplus,gpg482739qs5",
		.data = &giantplus_gpg482739qs5
	}, {
		.compatible = "hannstar,hsd070pww1",
		.data = &hannstar_hsd070pww1,
	}, {
		.compatible = "hannstar,hsd100pxn1",
		.data = &hannstar_hsd100pxn1,
	}, {
		.compatible = "hit,tx23d38vm0caa",
		.data = &hitachi_tx23d38vm0caa
	}, {
		.compatible = "innolux,at043tn24",
		.data = &innolux_at043tn24,
	}, {
		.compatible = "innolux,at070tn92",
		.data = &innolux_at070tn92,
	}, {
		.compatible ="innolux,g101ice-l01",
		.data = &innolux_g101ice_l01
	}, {
		.compatible ="innolux,g121i1-l01",
		.data = &innolux_g121i1_l01
	}, {
		.compatible = "innolux,g121x1-l03",
		.data = &innolux_g121x1_l03,
	}, {
		.compatible = "innolux,n116bge",
		.data = &innolux_n116bge,
	}, {
		.compatible = "innolux,n156bge-l21",
		.data = &innolux_n156bge_l21,
	}, {
		.compatible = "innolux,zj070na-01p",
		.data = &innolux_zj070na_01p,
	}, {
		.compatible = "jdi,tx26d202vm0bwa",
		.data = &jdi_tx26d202vm0bwa,
	}, {
		.compatible = "kyo,tcg121xglp",
		.data = &kyo_tcg121xglp,
	}, {
		.compatible = "lg,lb070wv8",
		.data = &lg_lb070wv8,
	}, {
		.compatible = "lg,lp079qx1-sp0v",
		.data = &lg_lp079qx1_sp0v,
	}, {
		.compatible = "lg,lp097qx1-spa1",
		.data = &lg_lp097qx1_spa1,
	}, {
		.compatible = "lg,lp120up1",
		.data = &lg_lp120up1,
	}, {
		.compatible = "lg,lp129qe",
		.data = &lg_lp129qe,
	}, {
		.compatible = "nec,nl12880bc20-05",
		.data = &nec_nl12880bc20_05,
	}, {
		.compatible = "nec,nl4827hc19-05b",
		.data = &nec_nl4827hc19_05b,
	}, {
		.compatible = "netron-dy,e231732",
		.data = &netron_dy_e231732,
	}, {
		.compatible = "nlt,nl192108ac18-02d",
		.data = &nlt_nl192108ac18_02d,
	}, {
		.compatible = "nvd,9128",
		.data = &nvd_9128,
	}, {
		.compatible = "okaya,rs800480t-7x0gp",
		.data = &okaya_rs800480t_7x0gp,
	}, {
		.compatible = "olimex,lcd-olinuxino-43-ts",
		.data = &olimex_lcd_olinuxino_43ts,
	}, {
		.compatible = "ontat,yx700wv03",
		.data = &ontat_yx700wv03,
	}, {
		.compatible = "ortustech,com43h4m85ulc",
		.data = &ortustech_com43h4m85ulc,
	}, {
		.compatible = "qiaodian,qd43003c0-40",
		.data = &qd43003c0_40,
	}, {
		.compatible = "samsung,lsn122dl01-c01",
		.data = &samsung_lsn122dl01_c01,
	}, {
		.compatible = "samsung,ltn101nt05",
		.data = &samsung_ltn101nt05,
	}, {
		.compatible = "samsung,ltn140at29-301",
		.data = &samsung_ltn140at29_301,
	}, {
		.compatible = "sharp,lq101k1ly04",
		.data = &sharp_lq101k1ly04,
	}, {
		.compatible = "sharp,lq123p1jx31",
		.data = &sharp_lq123p1jx31,
	}, {
		.compatible = "sharp,lq150x1lg11",
		.data = &sharp_lq150x1lg11,
	}, {
		.compatible = "shelly,sca07010-bfn-lnn",
		.data = &shelly_sca07010_bfn_lnn,
	}, {
		.compatible = "starry,kr122ea0sra",
		.data = &starry_kr122ea0sra,
	}, {
		.compatible = "tianma,tm070jdhg30",
		.data = &tianma_tm070jdhg30,
	}, {
		.compatible = "tpk,f07a-0102",
		.data = &tpk_f07a_0102,
	}, {
		.compatible = "tpk,f10a-0102",
		.data = &tpk_f10a_0102,
	}, {
		.compatible = "urt,umsh-8596md-t",
		.data = &urt_umsh_8596md_parallel,
	}, {
		.compatible = "urt,umsh-8596md-1t",
		.data = &urt_umsh_8596md_parallel,
	}, {
		.compatible = "urt,umsh-8596md-7t",
		.data = &urt_umsh_8596md_parallel,
	}, {
		.compatible = "urt,umsh-8596md-11t",
		.data = &urt_umsh_8596md_lvds,
	}, {
		.compatible = "urt,umsh-8596md-19t",
		.data = &urt_umsh_8596md_lvds,
	}, {
		.compatible = "urt,umsh-8596md-20t",
		.data = &urt_umsh_8596md_parallel,
	}, {
		.compatible = "winstar,wf35ltiacd",
		.data = &winstar_wf35ltiacd,
	}, {
		/* sentinel */
	}
};
MODULE_DEVICE_TABLE(of, platform_of_match);

static int panel_simple_platform_probe(struct platform_device *pdev)
{
	const struct of_device_id *id;

	id = of_match_node(platform_of_match, pdev->dev.of_node);
	if (!id)
		return -ENODEV;

	return panel_simple_probe(&pdev->dev, id->data, NULL);
}

static int panel_simple_platform_remove(struct platform_device *pdev)
{
	return panel_simple_remove(&pdev->dev);
}

static void panel_simple_platform_shutdown(struct platform_device *pdev)
{
	panel_simple_shutdown(&pdev->dev);
}

static struct platform_driver panel_simple_platform_driver = {
	.driver = {
		.name = "panel-simple",
		.of_match_table = platform_of_match,
	},
	.probe = panel_simple_platform_probe,
	.remove = panel_simple_platform_remove,
	.shutdown = panel_simple_platform_shutdown,
};

struct panel_desc_dsi {
	struct panel_desc desc;

	unsigned long flags;
	enum mipi_dsi_pixel_format format;
	unsigned int lanes;
};

static const struct drm_display_mode auo_b080uan01_mode = {
	.clock = 154500,
	.hdisplay = 1200,
	.hsync_start = 1200 + 62,
	.hsync_end = 1200 + 62 + 4,
	.htotal = 1200 + 62 + 4 + 62,
	.vdisplay = 1920,
	.vsync_start = 1920 + 9,
	.vsync_end = 1920 + 9 + 2,
	.vtotal = 1920 + 9 + 2 + 8,
	.vrefresh = 60,
};

static const struct panel_desc_dsi auo_b080uan01 = {
	.desc = {
		.modes = &auo_b080uan01_mode,
		.num_modes = 1,
		.bpc = 8,
		.size = {
			.width = 108,
			.height = 272,
		},
	},
	.flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_CLOCK_NON_CONTINUOUS,
	.format = MIPI_DSI_FMT_RGB888,
	.lanes = 4,
};

static const struct drm_display_mode boe_tv080wum_nl0_mode = {
	.clock = 160000,
	.hdisplay = 1200,
	.hsync_start = 1200 + 120,
	.hsync_end = 1200 + 120 + 20,
	.htotal = 1200 + 120 + 20 + 21,
	.vdisplay = 1920,
	.vsync_start = 1920 + 21,
	.vsync_end = 1920 + 21 + 3,
	.vtotal = 1920 + 21 + 3 + 18,
	.vrefresh = 60,
	.flags = DRM_MODE_FLAG_NVSYNC | DRM_MODE_FLAG_NHSYNC,
};

static const struct panel_desc_dsi boe_tv080wum_nl0 = {
	.desc = {
		.modes = &boe_tv080wum_nl0_mode,
		.num_modes = 1,
		.size = {
			.width = 107,
			.height = 172,
		},
	},
	.flags = MIPI_DSI_MODE_VIDEO |
		 MIPI_DSI_MODE_VIDEO_BURST |
		 MIPI_DSI_MODE_VIDEO_SYNC_PULSE,
	.format = MIPI_DSI_FMT_RGB888,
	.lanes = 4,
};

static const struct drm_display_mode lg_ld070wx3_sl01_mode = {
	.clock = 71000,
	.hdisplay = 800,
	.hsync_start = 800 + 32,
	.hsync_end = 800 + 32 + 1,
	.htotal = 800 + 32 + 1 + 57,
	.vdisplay = 1280,
	.vsync_start = 1280 + 28,
	.vsync_end = 1280 + 28 + 1,
	.vtotal = 1280 + 28 + 1 + 14,
	.vrefresh = 60,
};

static const struct panel_desc_dsi lg_ld070wx3_sl01 = {
	.desc = {
		.modes = &lg_ld070wx3_sl01_mode,
		.num_modes = 1,
		.bpc = 8,
		.size = {
			.width = 94,
			.height = 151,
		},
	},
	.flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_CLOCK_NON_CONTINUOUS,
	.format = MIPI_DSI_FMT_RGB888,
	.lanes = 4,
};

static const struct drm_display_mode lg_lh500wx1_sd03_mode = {
	.clock = 67000,
	.hdisplay = 720,
	.hsync_start = 720 + 12,
	.hsync_end = 720 + 12 + 4,
	.htotal = 720 + 12 + 4 + 112,
	.vdisplay = 1280,
	.vsync_start = 1280 + 8,
	.vsync_end = 1280 + 8 + 4,
	.vtotal = 1280 + 8 + 4 + 12,
	.vrefresh = 60,
};

static const struct panel_desc_dsi lg_lh500wx1_sd03 = {
	.desc = {
		.modes = &lg_lh500wx1_sd03_mode,
		.num_modes = 1,
		.bpc = 8,
		.size = {
			.width = 62,
			.height = 110,
		},
	},
	.flags = MIPI_DSI_MODE_VIDEO,
	.format = MIPI_DSI_FMT_RGB888,
	.lanes = 4,
};

static const struct drm_display_mode panasonic_vvx10f004b00_mode = {
	.clock = 157200,
	.hdisplay = 1920,
	.hsync_start = 1920 + 154,
	.hsync_end = 1920 + 154 + 16,
	.htotal = 1920 + 154 + 16 + 32,
	.vdisplay = 1200,
	.vsync_start = 1200 + 17,
	.vsync_end = 1200 + 17 + 2,
	.vtotal = 1200 + 17 + 2 + 16,
	.vrefresh = 60,
};

static const struct panel_desc_dsi panasonic_vvx10f004b00 = {
	.desc = {
		.modes = &panasonic_vvx10f004b00_mode,
		.num_modes = 1,
		.bpc = 8,
		.size = {
			.width = 217,
			.height = 136,
		},
	},
	.flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_SYNC_PULSE |
		 MIPI_DSI_CLOCK_NON_CONTINUOUS,
	.format = MIPI_DSI_FMT_RGB888,
	.lanes = 4,
};

static const struct of_device_id dsi_of_match[] = {
	{
		.compatible = "auo,b080uan01",
		.data = &auo_b080uan01
	}, {
		.compatible = "boe,tv080wum-nl0",
		.data = &boe_tv080wum_nl0
	}, {
		.compatible = "lg,ld070wx3-sl01",
		.data = &lg_ld070wx3_sl01
	}, {
		.compatible = "lg,lh500wx1-sd03",
		.data = &lg_lh500wx1_sd03
	}, {
		.compatible = "panasonic,vvx10f004b00",
		.data = &panasonic_vvx10f004b00
	}, {
		.compatible = "panel,simple",
		.data = NULL
	}, {
		/* sentinel */
	}
};
MODULE_DEVICE_TABLE(of, dsi_of_match);

static int panel_simple_dsi_probe(struct mipi_dsi_device *dsi)
{
	const struct panel_desc_dsi *desc;
	const struct of_device_id *id;
	const struct panel_desc *pd = NULL;
	int err;

	id = of_match_node(dsi_of_match, dsi->dev.of_node);
	if (!id)
		return -ENODEV;

	desc = id->data;

	if (desc) {
		dsi->mode_flags = desc->flags;
		dsi->format = desc->format;
		dsi->lanes = desc->lanes;
		pd = &desc->desc;
	} else {
		struct device_node *np = dsi->dev.of_node;
		const char *df;

		err = of_property_read_u32(np, "dsi-lanes", &dsi->lanes);
		if (err < 0) {
			dev_err(&dsi->dev, "Failed to get dsi-lanes property (%d)\n", err);
			return err;
		}
		err = of_property_read_string(np, "dsi-format", &df);
		if (err) {
			dev_err(&dsi->dev, "dsi-format missing. %d\n", err);
			return err;
		}
		if (!strcmp(df, "rgb888")) {
			dsi->format = MIPI_DSI_FMT_RGB888;
		} else if (!strcmp(df, "rgb666")) {
			dsi->format = MIPI_DSI_FMT_RGB666;
		} else {
			dev_err(&dsi->dev, "unknown dsi-format %s\n", df);
			return -EINVAL;
		}
		if (of_property_read_bool(np, "mode-clock-non-contiguous"))
			dsi->mode_flags |= MIPI_DSI_CLOCK_NON_CONTINUOUS;
		if (of_property_read_bool(np, "mode-skip-eot"))
			dsi->mode_flags |= MIPI_DSI_MODE_EOT_PACKET;
		if (of_property_read_bool(np, "mode-video"))
			dsi->mode_flags |= MIPI_DSI_MODE_VIDEO;
		if (of_property_read_bool(np, "mode-video-burst"))
			dsi->mode_flags |= MIPI_DSI_MODE_VIDEO_BURST;
		if (of_property_read_bool(np, "mode-video-hse"))
			dsi->mode_flags |= MIPI_DSI_MODE_VIDEO_HSE;
		if (of_property_read_bool(np, "mode-video-mbc"))
			dsi->mode_flags |= MIPI_DSI_MODE_VIDEO_MBC;
		if (of_property_read_bool(np, "mode-video-sync-pulse"))
			dsi->mode_flags |= MIPI_DSI_MODE_VIDEO_SYNC_PULSE;
	}
	err = panel_simple_probe(&dsi->dev, pd, dsi);
	if (err < 0)
		return err;

	return mipi_dsi_attach(dsi);
}

static int panel_simple_dsi_remove(struct mipi_dsi_device *dsi)
{
	int err;

	err = mipi_dsi_detach(dsi);
	if (err < 0)
		dev_err(&dsi->dev, "failed to detach from DSI host: %d\n", err);

	return panel_simple_remove(&dsi->dev);
}

static void panel_simple_dsi_shutdown(struct mipi_dsi_device *dsi)
{
	panel_simple_shutdown(&dsi->dev);
}

static struct mipi_dsi_driver panel_simple_dsi_driver = {
	.driver = {
		.name = "panel-simple-dsi",
		.of_match_table = dsi_of_match,
	},
	.probe = panel_simple_dsi_probe,
	.remove = panel_simple_dsi_remove,
	.shutdown = panel_simple_dsi_shutdown,
};

static int __init panel_simple_init(void)
{
	int err;

	err = platform_driver_register(&panel_simple_platform_driver);
	if (err < 0)
		return err;

	if (IS_ENABLED(CONFIG_DRM_MIPI_DSI)) {
		err = mipi_dsi_driver_register(&panel_simple_dsi_driver);
		if (err < 0)
			return err;
	}

	return 0;
}
module_init(panel_simple_init);

static void __exit panel_simple_exit(void)
{
	if (IS_ENABLED(CONFIG_DRM_MIPI_DSI))
		mipi_dsi_driver_unregister(&panel_simple_dsi_driver);

	platform_driver_unregister(&panel_simple_platform_driver);
}
module_exit(panel_simple_exit);

MODULE_AUTHOR("Thierry Reding <treding@nvidia.com>");
MODULE_DESCRIPTION("DRM Driver for Simple Panels");
MODULE_LICENSE("GPL and additional rights");
