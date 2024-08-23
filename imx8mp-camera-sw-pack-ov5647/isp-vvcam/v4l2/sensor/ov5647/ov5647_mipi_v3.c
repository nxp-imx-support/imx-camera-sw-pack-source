/*
 * Copyright (C) 2012-2015 Freescale Semiconductor, Inc. All Rights Reserved.
 * Copyright (c) 2020 VeriSilicon Holdings Co., Ltd.
 * Copyright 2023-2024 NXP
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/of_graph.h>
#include <linux/device.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/pinctrl/consumer.h>
#include <linux/regulator/consumer.h>
#include <linux/v4l2-mediabus.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-fwnode.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include "vvsensor.h"

#include "ov5647_regs_1080p.h"

#define OV5647_VOLTAGE_ANALOG			2800000
#define OV5647_VOLTAGE_DIGITAL_CORE		1500000
#define OV5647_VOLTAGE_DIGITAL_IO		1800000

#define OV5647_XCLK_MIN 6000000
#define OV5647_XCLK_MAX 24000000

#define OV5647_SENS_PAD_SOURCE	0
#define OV5647_SENS_PADS_NUM	1

#define client_to_ov5647(client)\
	container_of(i2c_get_clientdata(client), struct ov5647, subdev)

/*
Use USER_TO_KERNEL/KERNEL_TO_USER to fix "uaccess" exception on run time.
Also, use "copy_ret" to fix the build issue as below.
error: ignoring return value of function declared with 'warn_unused_result' attribute.
*/

#ifdef CONFIG_HARDENED_USERCOPY
#define USER_TO_KERNEL(TYPE) \
	do {\
		TYPE tmp; \
		unsigned long copy_ret; \
		arg = (void *)(&tmp); \
		copy_ret = copy_from_user(arg, arg_user, sizeof(TYPE));\
	} while (0)

#define KERNEL_TO_USER(TYPE) \
	do {\
		unsigned long copy_ret; \
		copy_ret = copy_to_user(arg_user, arg, sizeof(TYPE));\
	} while (0)
#else
#define USER_TO_KERNEL(TYPE)
#define KERNEL_TO_USER(TYPE)
#endif


struct ov5647_capture_properties {
	__u64 max_lane_frequency;
	__u64 max_pixel_frequency;
	__u64 max_data_rate;
};

struct ov5647 {
	struct i2c_client *i2c_client;
	struct regulator *io_regulator;
	struct regulator *core_regulator;
	struct regulator *analog_regulator;
	unsigned int pwn_gpio;
	unsigned int rst_gpio;
	unsigned int mclk;
	unsigned int mclk_source;
	struct clk *sensor_clk;
	u32 clk_freq;
	unsigned int csi_id;
	struct ov5647_capture_properties ocp;

	struct v4l2_subdev subdev;
	struct media_pad pads[OV5647_SENS_PADS_NUM];

	struct v4l2_mbus_framefmt format;
	vvcam_mode_info_t cur_mode;
	sensor_blc_t blc;
	sensor_white_balance_t wb;
	struct mutex lock;
	u32 stream_status;
	u32 resume_status;
};

static struct vvcam_mode_info_s pov5647_mode_info[] = {
	{
		.index	        = 0,
		.size           = {
			.bounds_width  = 1920,
			.bounds_height = 1080,
			.top           = 0,
			.left          = 0,
			.width         = 1920,
			.height        = 1080,
		},
		.hdr_mode       = SENSOR_MODE_LINEAR,
		.bit_width      = 10,
		.data_compress  = {
			.enable = 0,
		},
		.bayer_pattern  = BAYER_BGGR,
		.ae_info = {
			.def_frm_len_lines     = 0x450,
			.curr_frm_len_lines    = 0x450,
			.one_line_exp_time_ns  = 30193,
			.max_integration_line  = 0x450 - 16,
			.min_integration_line  = 16,
			.max_again             = 63.5 * (1 << SENSOR_FIX_FRACBITS),
			.min_again             = 1    * (1 << SENSOR_FIX_FRACBITS),
			.max_dgain             = 1    * (1 << SENSOR_FIX_FRACBITS), 
			.min_dgain             = 1    * (1 << SENSOR_FIX_FRACBITS),
			.start_exposure        = 2 * 100 * (1 << SENSOR_FIX_FRACBITS),
			.cur_fps               = 30   * (1 << SENSOR_FIX_FRACBITS),
			.max_fps               = 30   * (1 << SENSOR_FIX_FRACBITS),
			.min_fps               = 1    * (1 << SENSOR_FIX_FRACBITS),
			.min_afps              = 5    * (1 << SENSOR_FIX_FRACBITS),
			.int_update_delay_frm  = 1,
			.gain_update_delay_frm = 1,
		},
		.mipi_info = {
			.mipi_lane = 2,
		},
		.preg_data      = ov5647_init_setting_1080p,
		.reg_data_count = ARRAY_SIZE(ov5647_init_setting_1080p),
	},
};

static int ov5647_power_on(struct ov5647 *sensor)
{
	int ret;
	pr_debug("enter %s\n", __func__);

	if (gpio_is_valid(sensor->pwn_gpio))
		gpio_set_value_cansleep(sensor->pwn_gpio, 1);

	ret = clk_prepare_enable(sensor->sensor_clk);
	if (ret < 0)
		pr_err("%s: enable sensor clk fail\n", __func__);

	return ret;
}

static int ov5647_power_off(struct ov5647 *sensor)
{
	pr_debug("enter %s\n", __func__);
	if (gpio_is_valid(sensor->pwn_gpio))
		gpio_set_value_cansleep(sensor->pwn_gpio, 0);
	clk_disable_unprepare(sensor->sensor_clk);

	return 0;
}

static int ov5647_s_power(struct v4l2_subdev *sd, int on)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct ov5647 *sensor = client_to_ov5647(client);

	pr_debug("enter %s\n", __func__);
	if (on)
		ov5647_power_on(sensor);
	else
		ov5647_power_off(sensor);

	return 0;
}

static int ov5647_get_clk(struct ov5647 *sensor, void *clk)
{
	struct vvcam_clk_s vvcam_clk;
	int ret = 0;
	vvcam_clk.sensor_mclk = clk_get_rate(sensor->sensor_clk);
	vvcam_clk.csi_max_pixel_clk = sensor->ocp.max_pixel_frequency;
	ret = copy_to_user(clk, &vvcam_clk, sizeof(struct vvcam_clk_s));
	if (ret != 0)
		ret = -EINVAL;
	return ret;
}

static int ov5647_write_reg(struct ov5647 *sensor, u16 reg, u8 val)
{
	struct device *dev = &sensor->i2c_client->dev;
	u8 au8Buf[3] = { 0 };

	au8Buf[0] = reg >> 8;
	au8Buf[1] = reg & 0xff;
	au8Buf[2] = val;

	if (i2c_master_send(sensor->i2c_client, au8Buf, 3) < 0) {
		dev_err(dev, "Write reg error: reg=%x, val=%x\n", reg, val);
		return -1;
	}

	return 0;
}

static int ov5647_read_reg(struct ov5647 *sensor, u16 reg, u8 *val)
{
	struct device *dev = &sensor->i2c_client->dev;
	u8 au8RegBuf[2] = { 0 };
	u8 u8RdVal = 0;

	au8RegBuf[0] = reg >> 8;
	au8RegBuf[1] = reg & 0xff;

	if (i2c_master_send(sensor->i2c_client, au8RegBuf, 2) != 2) {
		dev_err(dev, "Read reg error: reg=%x\n", reg);
		return -1;
	}

	if (i2c_master_recv(sensor->i2c_client, &u8RdVal, 1) != 1) {
		dev_err(dev, "Read reg error: reg=%x, val=%x\n", reg, u8RdVal);
		return -1;
	}

	*val = u8RdVal;

	return 0;
}

static int ov5647_write_reg_arry(struct ov5647 *sensor,
				  struct vvcam_sccb_data_s *reg_arry,
				  u32 size)
{
	int i = 0;
	int ret = 0;
	struct i2c_msg msg;
	u8 *send_buf;
	u32 send_buf_len = 0;
	struct i2c_client *i2c_client = sensor->i2c_client;

	send_buf = (u8 *)kmalloc(size + 2, GFP_KERNEL);
	if (!send_buf)
		return -ENOMEM;

	send_buf[send_buf_len++] = (reg_arry[0].addr >> 8) & 0xff;
	send_buf[send_buf_len++] = reg_arry[0].addr & 0xff;
	send_buf[send_buf_len++] = reg_arry[0].data & 0xff;
	for (i=1; i < size; i++) {
		if (reg_arry[i].addr == (reg_arry[i-1].addr + 1)){
			send_buf[send_buf_len++] = reg_arry[i].data & 0xff;
		} else {
			msg.addr  = i2c_client->addr;
			msg.flags = i2c_client->flags;
			msg.buf   = send_buf;
			msg.len   = send_buf_len;
			ret = i2c_transfer(i2c_client->adapter, &msg, 1);
			if (ret < 0) {
				pr_err("%s:i2c transfer error\n",__func__);
				kfree(send_buf);
				return ret;
			}
			send_buf_len = 0;
			send_buf[send_buf_len++] =
				(reg_arry[i].addr >> 8) & 0xff;
			send_buf[send_buf_len++] =
				reg_arry[i].addr & 0xff;
			send_buf[send_buf_len++] =
				reg_arry[i].data & 0xff;
		}
	}

	if (send_buf_len > 0) {
		msg.addr  = i2c_client->addr;
		msg.flags = i2c_client->flags;
		msg.buf   = send_buf;
		msg.len   = send_buf_len;
		ret = i2c_transfer(i2c_client->adapter, &msg, 1);
		if (ret < 0)
			pr_err("%s:i2c transfer end meg error\n",__func__);
		else
			ret = 0;

	}
	kfree(send_buf);
	return ret;
}

static int ov5647_query_capability(struct ov5647 *sensor, void *arg)
{
	struct v4l2_capability *pcap = (struct v4l2_capability *)arg;

	strcpy((char *)pcap->driver, "ov5647");
	sprintf((char *)pcap->bus_info, "csi%d",sensor->csi_id);
	if(sensor->i2c_client->adapter) {
		pcap->bus_info[VVCAM_CAP_BUS_INFO_I2C_ADAPTER_NR_POS] =
			(__u8)sensor->i2c_client->adapter->nr;
	} else {
		pcap->bus_info[VVCAM_CAP_BUS_INFO_I2C_ADAPTER_NR_POS] = 0xFF;
	}

	return 0;
}

static int ov5647_query_supports(struct ov5647 *sensor, void* parry)
{
	struct vvcam_mode_info_array_s *psensor_mode_arry = parry;

	psensor_mode_arry->count = ARRAY_SIZE(pov5647_mode_info);
	memcpy((void *)&psensor_mode_arry->modes, (void *)pov5647_mode_info, sizeof(pov5647_mode_info));

	return 0;
}

static int ov5647_get_sensor_id(struct ov5647 *sensor, void* pchip_id)
{
	int ret = 0;
	u32 chip_id;
	u8 chip_id_high = 0;
	u8 chip_id_low = 0;
	ret  = ov5647_read_reg(sensor, 0x300a, &chip_id_high);
	ret |= ov5647_read_reg(sensor, 0x300b, &chip_id_low);

	chip_id = ((chip_id_high & 0xff) << 8) | 
	        (chip_id_low & 0xff);

	ret = copy_to_user(pchip_id, &chip_id, sizeof(u32));
	if (ret != 0)
		ret = -ENOMEM;
	return ret;
}

static int ov5647_get_reserve_id(struct ov5647 *sensor, void* preserve_id)
{
	int ret = 0;
	u32 reserve_id = 0x5647;
	ret = copy_to_user(preserve_id, &reserve_id, sizeof(u32));
	if (ret != 0)
		ret = -ENOMEM;
	return ret;
}

static int ov5647_get_sensor_mode(struct ov5647 *sensor, void* pmode)
{
	int ret = 0;
	ret = copy_to_user(pmode, &sensor->cur_mode,
		sizeof(struct vvcam_mode_info_s));
	if (ret != 0)
		ret = -ENOMEM;
	return ret;
}

static int ov5647_set_sensor_mode(struct ov5647 *sensor, void* pmode)
{
	int ret = 0;
	int i = 0;
	struct vvcam_mode_info_s sensor_mode;
	ret = copy_from_user(&sensor_mode, pmode,
		sizeof(struct vvcam_mode_info_s));
	if (ret != 0)
		return -ENOMEM;
	for (i = 0; i < ARRAY_SIZE(pov5647_mode_info); i++) {
		if (pov5647_mode_info[i].index == sensor_mode.index) {
			memcpy(&sensor->cur_mode, &pov5647_mode_info[i],
				sizeof(struct vvcam_mode_info_s));
			return 0;
		}
	}

	return -ENXIO;
}

static int ov5647_set_exp(struct ov5647 *sensor, u32 exp)
{
	int ret = 0;
	u32 val_exp = 0;

	val_exp = exp * 16;

	ret |= ov5647_write_reg(sensor, 0x3500, (val_exp >> 16) & 0xff);
	ret |= ov5647_write_reg(sensor, 0x3501, (val_exp >> 8) & 0xff);
	ret |= ov5647_write_reg(sensor, 0x3502, val_exp & 0xff);


	return ret;
}

static int ov5647_set_vsexp(struct ov5647 *sensor, u32 exp)
{
	int ret = 0;
	u32 val_exp = 0;

	val_exp = exp * 16;

	ret |= ov5647_write_reg(sensor, 0x3500, (val_exp >> 16) & 0xff);
	ret |= ov5647_write_reg(sensor, 0x3501, (val_exp >> 8) & 0xff);
	ret |= ov5647_write_reg(sensor, 0x3502, val_exp & 0xff);

	return ret;
}

static int ov5647_set_gain(struct ov5647 *sensor, u32 total_gain)
{
	int ret = 0;
	u32 again = 0;

	again = (total_gain * 16) / (1 << SENSOR_FIX_FRACBITS);

	ret |= ov5647_write_reg(sensor, 0x350a, (again >> 8) & 0xff);
	ret |= ov5647_write_reg(sensor, 0x350b, again & 0xff);

	return ret;
}

static int ov5647_set_vsgain(struct ov5647 *sensor, u32 total_gain)
{
	int ret = 0;
	u32 again = 0;

	again = (total_gain * 16) / (1 << SENSOR_FIX_FRACBITS);

	ret |= ov5647_write_reg(sensor, 0x350a, (again >> 8) & 0xff);
	ret |= ov5647_write_reg(sensor, 0x350b, again & 0xff);

	return ret;
}

static int ov5647_get_fps(struct ov5647 *sensor, u32 *pfps)
{
	*pfps = sensor->cur_mode.ae_info.cur_fps;
	return 0;
}

static int ov5647_set_test_pattern(struct ov5647 *sensor, void * arg)
{
	int ret;
	struct sensor_test_pattern_s test_pattern;

	ret = copy_from_user(&test_pattern, arg, sizeof(test_pattern));
	if (ret != 0)
		return -ENOMEM;
	if (test_pattern.enable) {
		switch (test_pattern.pattern) {
		case 0:
			ov5647_write_reg(sensor, 0x503d, 0x80);
			break;
		case 1:
			ov5647_write_reg(sensor, 0x503d, 0x81);
			break;
		case 2:
			ov5647_write_reg(sensor, 0x503d, 0x82);
			break;
		case 3:
			ov5647_write_reg(sensor, 0x503d, 0x88);
			break;
		case 4:
			ov5647_write_reg(sensor, 0x503d, 0xc4);
			break;
		default:
			ret = -1;
			break;
		}
	} else {
		ov5647_write_reg(sensor, 0x503d, 0x00);	
	}
	return ret;
}

static int ov5647_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct ov5647 *sensor = client_to_ov5647(client);

	if (enable)
		ov5647_write_reg(sensor, 0x0100, 0x01);
	else
		ov5647_write_reg(sensor, 0x0100, 0x00);

	sensor->stream_status = enable;
	return 0;
}

static int ov5647_get_format_code(struct ov5647 *sensor, u32 *code)
{
	switch (sensor->cur_mode.bayer_pattern) {
	case BAYER_RGGB:
		if (sensor->cur_mode.bit_width == 8) {
			*code = MEDIA_BUS_FMT_SRGGB8_1X8;
		} else if (sensor->cur_mode.bit_width == 10) {
			*code = MEDIA_BUS_FMT_SRGGB10_1X10;
		} else {
			*code = MEDIA_BUS_FMT_SRGGB12_1X12;
		}
		break;
	case BAYER_GRBG:
		if (sensor->cur_mode.bit_width == 8) {
			*code = MEDIA_BUS_FMT_SGRBG8_1X8;
		} else if (sensor->cur_mode.bit_width == 10) {
			*code = MEDIA_BUS_FMT_SGRBG10_1X10;
		} else {
			*code = MEDIA_BUS_FMT_SGRBG12_1X12;
		}
		break;
	case BAYER_GBRG:
		if (sensor->cur_mode.bit_width == 8) {
			*code = MEDIA_BUS_FMT_SGBRG8_1X8;
		} else if (sensor->cur_mode.bit_width == 10) {
			*code = MEDIA_BUS_FMT_SGBRG10_1X10;
		} else {
			*code = MEDIA_BUS_FMT_SGBRG12_1X12;
		}
		break;
	case BAYER_BGGR:
		if (sensor->cur_mode.bit_width == 8) {
			*code = MEDIA_BUS_FMT_SBGGR8_1X8;
		} else if (sensor->cur_mode.bit_width == 10) {
			*code = MEDIA_BUS_FMT_SBGGR10_1X10;
		} else {
			*code = MEDIA_BUS_FMT_SBGGR12_1X12;
		}
		break;
	default:
		/*nothing need to do*/
		break;
	}
	return 0;
}
#if LINUX_VERSION_CODE > KERNEL_VERSION(5, 12, 0)
static int ov5647_enum_mbus_code(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *state,
				struct v4l2_subdev_mbus_code_enum *code)
#else
static int ov5647_enum_mbus_code(struct v4l2_subdev *sd,
			         struct v4l2_subdev_pad_config *cfg,
			         struct v4l2_subdev_mbus_code_enum *code)
#endif
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct ov5647 *sensor = client_to_ov5647(client);

	u32 cur_code = MEDIA_BUS_FMT_SBGGR10_1X10;

	if (code->index > 0)
		return -EINVAL;
	ov5647_get_format_code(sensor,&cur_code);
	code->code = cur_code;

	return 0;
}
#if LINUX_VERSION_CODE > KERNEL_VERSION(5, 12, 0)
static int ov5647_set_fmt(struct v4l2_subdev *sd,
			   struct v4l2_subdev_state *state,
			   struct v4l2_subdev_format *fmt)
#else
static int ov5647_set_fmt(struct v4l2_subdev *sd,
			   struct v4l2_subdev_pad_config *cfg,
			   struct v4l2_subdev_format *fmt)
#endif
{
	int ret = 0;
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct ov5647 *sensor = client_to_ov5647(client);
	mutex_lock(&sensor->lock);

	if ((fmt->format.width != sensor->cur_mode.size.bounds_width) ||
	    (fmt->format.height != sensor->cur_mode.size.bounds_height)) {
		pr_err("%s:set sensor format %dx%d error\n",
			__func__,fmt->format.width,fmt->format.height);
		mutex_unlock(&sensor->lock);
		return -EINVAL;
	}

	ov5647_write_reg(sensor, 0x0100, 0x00);
	ov5647_write_reg(sensor, 0x0103, 0x01);
	msleep(20);

	ret = ov5647_write_reg_arry(sensor,
		(struct vvcam_sccb_data_s *)sensor->cur_mode.preg_data,
		sensor->cur_mode.reg_data_count);
	if (ret < 0) {
		pr_err("%s:ov5647_write_reg_arry error\n",__func__);
		mutex_unlock(&sensor->lock);
		return -EINVAL;
	}

	ov5647_get_format_code(sensor, &fmt->format.code);
	fmt->format.field = V4L2_FIELD_NONE;
	sensor->format = fmt->format;
	mutex_unlock(&sensor->lock);
	return 0;
}
#if LINUX_VERSION_CODE > KERNEL_VERSION(5, 12, 0)
static int ov5647_get_fmt(struct v4l2_subdev *sd,
			   struct v4l2_subdev_state *state,
			   struct v4l2_subdev_format *fmt)
#else
static int ov5647_get_fmt(struct v4l2_subdev *sd,
			   struct v4l2_subdev_pad_config *cfg,
			   struct v4l2_subdev_format *fmt)
#endif
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct ov5647 *sensor = client_to_ov5647(client);

	mutex_lock(&sensor->lock);
	fmt->format = sensor->format;
	mutex_unlock(&sensor->lock);
	return 0;
}

static long ov5647_priv_ioctl(struct v4l2_subdev *sd,
                              unsigned int cmd,
                              void *arg_user)
{
	struct i2c_client *client = v4l2_get_subdevdata(sd);
	struct ov5647 *sensor = client_to_ov5647(client);
	long ret = 0;
	struct vvcam_sccb_data_s sensor_reg;
	void *arg = arg_user;

	mutex_lock(&sensor->lock);
	switch (cmd){
	case VVSENSORIOC_S_POWER:
		ret = 0;
		break;
	case VVSENSORIOC_S_CLK:
		ret = 0;
		break;
	case VVSENSORIOC_G_CLK:
		ret = ov5647_get_clk(sensor,arg);
		break;
	case VVSENSORIOC_RESET:
		ret = 0;
		break;
	case VIDIOC_QUERYCAP:
		ret = ov5647_query_capability(sensor, arg);
		break;
	case VVSENSORIOC_QUERY:
		USER_TO_KERNEL(struct vvcam_mode_info_array_s);
		ret = ov5647_query_supports(sensor, arg);
		KERNEL_TO_USER(struct vvcam_mode_info_array_s);
		break;
	case VVSENSORIOC_G_CHIP_ID:
		ret = ov5647_get_sensor_id(sensor, arg);
		break;
	case VVSENSORIOC_G_RESERVE_ID:
		ret = ov5647_get_reserve_id(sensor, arg);
		break;
	case VVSENSORIOC_G_SENSOR_MODE:
		ret = ov5647_get_sensor_mode(sensor, arg);
		break;
	case VVSENSORIOC_S_SENSOR_MODE:
		ret = ov5647_set_sensor_mode(sensor, arg);
		break;
	case VVSENSORIOC_S_STREAM:
		USER_TO_KERNEL(int);
		ret = ov5647_s_stream(&sensor->subdev, *(int *)arg);
		break;
	case VVSENSORIOC_WRITE_REG:
		ret = copy_from_user(&sensor_reg, arg,
			sizeof(struct vvcam_sccb_data_s));
		ret |= ov5647_write_reg(sensor, sensor_reg.addr,
			sensor_reg.data);
		break;
	case VVSENSORIOC_READ_REG:
		ret = copy_from_user(&sensor_reg, arg,
			sizeof(struct vvcam_sccb_data_s));
		ret |= ov5647_read_reg(sensor, sensor_reg.addr,
			(u8 *)&sensor_reg.data);
		ret |= copy_to_user(arg, &sensor_reg,
			sizeof(struct vvcam_sccb_data_s));
		break;
	case VVSENSORIOC_S_EXP:
		USER_TO_KERNEL(u32);
		ret = ov5647_set_exp(sensor, *(u32 *)arg);
		break;
	case VVSENSORIOC_S_VSEXP:
		USER_TO_KERNEL(u32);
		ret = ov5647_set_vsexp(sensor, *(u32 *)arg);
		break;
	case VVSENSORIOC_S_GAIN:
		USER_TO_KERNEL(u32);
		ret = ov5647_set_gain(sensor, *(u32 *)arg);
		break;
	case VVSENSORIOC_S_VSGAIN:
		USER_TO_KERNEL(u32);
		ret = ov5647_set_vsgain(sensor, *(u32 *)arg);
		break;
	case VVSENSORIOC_S_FPS:
		USER_TO_KERNEL(u32);
		//ret = ov5647_set_fps(sensor, *(u32 *)arg);
		break;
	case VVSENSORIOC_G_FPS:
		USER_TO_KERNEL(u32);
		ret = ov5647_get_fps(sensor, (u32 *)arg);
		KERNEL_TO_USER(u32);
		break;
	case VVSENSORIOC_S_HDR_RADIO:
		//ret = ov5647_set_ratio(sensor, arg);
		break;
	case VVSENSORIOC_S_TEST_PATTERN:
		ret= ov5647_set_test_pattern(sensor, arg);
		break;
	default:
		break;
	}

	mutex_unlock(&sensor->lock);
	return ret;
}

static struct v4l2_subdev_video_ops ov5647_subdev_video_ops = {
	.s_stream = ov5647_s_stream,
};

static const struct v4l2_subdev_pad_ops ov5647_subdev_pad_ops = {
	.enum_mbus_code = ov5647_enum_mbus_code,
	.set_fmt = ov5647_set_fmt,
	.get_fmt = ov5647_get_fmt,
};

static struct v4l2_subdev_core_ops ov5647_subdev_core_ops = {
	.s_power = ov5647_s_power,
	.ioctl = ov5647_priv_ioctl,
};

static struct v4l2_subdev_ops ov5647_subdev_ops = {
	.core  = &ov5647_subdev_core_ops,
	.video = &ov5647_subdev_video_ops,
	.pad   = &ov5647_subdev_pad_ops,
};

static int ov5647_link_setup(struct media_entity *entity,
			     const struct media_pad *local,
			     const struct media_pad *remote, u32 flags)
{
	return 0;
}

static const struct media_entity_operations ov5647_sd_media_ops = {
	.link_setup = ov5647_link_setup,
};

static void ov5647_reset(struct ov5647 *sensor)
{
	pr_debug("enter %s\n", __func__);
	if (!gpio_is_valid(sensor->rst_gpio))
		return;

	gpio_set_value_cansleep(sensor->rst_gpio, 0);
	msleep(20);

	gpio_set_value_cansleep(sensor->rst_gpio, 1);
	msleep(20);

	return;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 3, 0)
static int ov5647_probe(struct i2c_client *client)
#else
static int ov5647_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
#endif
{
	int retval;
	struct device *dev = &client->dev;
	struct v4l2_subdev *sd;
	struct ov5647 *sensor;
	u32 chip_id = 0;
	u8 reg_val = 0;

	pr_info("enter %s\n", __func__);

	sensor = devm_kmalloc(dev, sizeof(*sensor), GFP_KERNEL);
	if (!sensor)
		return -ENOMEM;
	memset(sensor, 0, sizeof(*sensor));

	sensor->i2c_client = client;

	sensor->pwn_gpio = of_get_named_gpio(dev->of_node, "pwn-gpios", 0);
	if (!gpio_is_valid(sensor->pwn_gpio))
		dev_warn(dev, "No sensor pwdn pin available");
	else {
		retval = devm_gpio_request_one(dev, sensor->pwn_gpio,
						GPIOF_OUT_INIT_HIGH,
						"ov5647_mipi_pwdn");
		if (retval < 0) {
			dev_warn(dev, "Failed to set power pin\n");
			dev_warn(dev, "retval=%d\n", retval);
			return retval;
		}
	}

	sensor->rst_gpio = of_get_named_gpio(dev->of_node, "rst-gpios", 0);
	if (!gpio_is_valid(sensor->rst_gpio))
		dev_warn(dev, "No sensor reset pin available");
	else {
		retval = devm_gpio_request_one(dev, sensor->rst_gpio,
						GPIOF_OUT_INIT_HIGH,
						"ov5647_mipi_reset");
		if (retval < 0) {
			dev_warn(dev, "Failed to set reset pin\n");
			return retval;
		}
	}

	sensor->sensor_clk = devm_clk_get(dev, NULL);
	if (IS_ERR(sensor->sensor_clk)) {
		sensor->sensor_clk = NULL;
		dev_err(dev, "clock-frequency missing or invalid\n");
		return PTR_ERR(sensor->sensor_clk);
	}

	sensor->clk_freq = clk_get_rate(sensor->sensor_clk);
	if (sensor->clk_freq != 24000000) {
		dev_err(dev, "clk frequency not supported: %d Hz\n",
			sensor->clk_freq);
		return -EINVAL;
	}

	retval = of_property_read_u32(dev->of_node, "csi_id", &(sensor->csi_id));
	if (retval) {
		dev_err(dev, "csi id missing or invalid\n");
		return retval;
	}

	retval = ov5647_power_on(sensor);
	if (retval < 0) {
		dev_err(dev, "%s: sensor power on fail\n", __func__);
	}

	ov5647_reset(sensor);

	ov5647_read_reg(sensor, 0x300a, &reg_val);
	chip_id |= reg_val << 8;
	ov5647_read_reg(sensor, 0x300b, &reg_val);
	chip_id |= reg_val;
	if (chip_id != 0x5647) {
		pr_warn("camera ov5647 is not found\n");
		retval = -ENODEV;
		goto probe_err_power_off;
	}

	sd = &sensor->subdev;
	v4l2_i2c_subdev_init(sd, client, &ov5647_subdev_ops);
	sd->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	sd->dev = &client->dev;
	sd->entity.ops = &ov5647_sd_media_ops;
	sd->entity.function = MEDIA_ENT_F_CAM_SENSOR;
	sensor->pads[OV5647_SENS_PAD_SOURCE].flags = MEDIA_PAD_FL_SOURCE;
	retval = media_entity_pads_init(&sd->entity,
				OV5647_SENS_PADS_NUM,
				sensor->pads);
	if (retval < 0)
		goto probe_err_power_off;
#if LINUX_VERSION_CODE > KERNEL_VERSION(5, 12, 0)
	retval = v4l2_async_register_subdev_sensor(sd);
#else
	retval = v4l2_async_register_subdev_sensor_common(sd);
#endif
	if (retval < 0) {
		dev_err(&client->dev,"%s--Async register failed, ret=%d\n",
			__func__,retval);
		goto probe_err_free_entiny;
	}

	memcpy(&sensor->cur_mode, &pov5647_mode_info[0],
			sizeof(struct vvcam_mode_info_s));

	mutex_init(&sensor->lock);
	pr_info("%s camera mipi ov5647, is found\n", __func__);

	return 0;

probe_err_free_entiny:
	media_entity_cleanup(&sd->entity);

probe_err_power_off:
	ov5647_power_off(sensor);

	return retval;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 0, 0)
static int ov5647_remove(struct i2c_client *client)
#else
static void ov5647_remove(struct i2c_client *client)
#endif
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct ov5647 *sensor = client_to_ov5647(client);

	pr_info("enter %s\n", __func__);

	v4l2_async_unregister_subdev(sd);
	media_entity_cleanup(&sd->entity);
	ov5647_power_off(sensor);
	mutex_destroy(&sensor->lock);

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 0, 0)
	return 0;
#else
#endif
}

static int __maybe_unused ov5647_suspend(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct ov5647 *sensor = client_to_ov5647(client);

	sensor->resume_status = sensor->stream_status;
	if (sensor->resume_status) {
		ov5647_s_stream(&sensor->subdev,0);
	}

	return 0;
}

static int __maybe_unused ov5647_resume(struct device *dev)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct ov5647 *sensor = client_to_ov5647(client);

	if (sensor->resume_status) {
		ov5647_s_stream(&sensor->subdev,1);
	}

	return 0;
}

static const struct dev_pm_ops ov5647_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(ov5647_suspend, ov5647_resume)
};

static const struct i2c_device_id ov5647_id[] = {
	{"ov5647", 0},
	{},
};
MODULE_DEVICE_TABLE(i2c, ov5647_id);

static const struct of_device_id ov5647_of_match[] = {
	{ .compatible = "ovti,ov5647" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ov5647_of_match);

static struct i2c_driver ov5647_i2c_driver = {
	.driver = {
		.owner = THIS_MODULE,
		.name  = "ov5647",
		.pm = &ov5647_pm_ops,
		.of_match_table	= ov5647_of_match,
	},
	.probe	= ov5647_probe,
	.remove = ov5647_remove,
	.id_table = ov5647_id,
};

module_i2c_driver(ov5647_i2c_driver);
MODULE_DESCRIPTION("OV5647 MIPI Camera Subdev Driver");
MODULE_LICENSE("GPL");
