/*
 * Copyright (c) 2015 MediaTek Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/backlight.h>
#include <drm/drmP.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_panel.h>

#include <linux/gpio/consumer.h> 
#include <linux/regulator/consumer.h>

#include <video/mipi_display.h>
#include <video/of_videomode.h>
#include <video/videomode.h>

#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/of_graph.h>
#include <linux/platform_device.h>

#define CONFIG_MTK_PANEL_EXT
#if defined(CONFIG_MTK_PANEL_EXT)
#include "../mediatek/mtk_panel_ext.h"
#include "../mediatek/mtk_log.h"
#include "../mediatek/mtk_drm_graphics_base.h"
#endif

#include <linux/i2c-dev.h>
#include <linux/i2c.h>

#ifdef CONFIG_MTK_ROUND_CORNER_SUPPORT
#include "../mediatek/mtk_corner_pattern/mtk_data_hw_roundedpattern.h"
#endif
#include "include/panel-nt37700c-vdo-120hz-vfp-6382.h"
/* i2c control start */
#define LCM_I2C_ID_NAME "I2C_LCD_BIAS"
static struct i2c_client *_lcm_i2c_client;


//prize add by wangfei for lcd hardware info 20210726 start
#if defined(CONFIG_PRIZE_HARDWARE_INFO)
#include "../../../misc/mediatek/prize/hardware_info/hardware_info.h"
extern struct hardware_info current_lcm_info;
#endif
//prize add by wangfei for lcd hardware info 20210726 end


//prize add by wangfei for HBM 20210906 start
unsigned int bl_level;
//prize add by wangfei for HBM 20210906 end
unsigned int last_bl_level;
struct lcm *g_ctx;
/*****************************************************************************
 * Function Prototype
 *****************************************************************************/
static int _lcm_i2c_probe(struct i2c_client *client,
			  const struct i2c_device_id *id);
static int _lcm_i2c_remove(struct i2c_client *client);

/*****************************************************************************
 * Data Structure
 *****************************************************************************/
struct _lcm_i2c_dev {
	struct i2c_client *client;
};

static const struct of_device_id _lcm_i2c_of_match[] = {
	{
	    .compatible = "mediatek,i2c_lcd_bias",
	},
	{},
};

static const struct i2c_device_id _lcm_i2c_id[] = { { LCM_I2C_ID_NAME, 0 },
						    {} };

static struct i2c_driver _lcm_i2c_driver = {
	.id_table = _lcm_i2c_id,
	.probe = _lcm_i2c_probe,
	.remove = _lcm_i2c_remove,
	/* .detect		   = _lcm_i2c_detect, */
	.driver = {
		.owner = THIS_MODULE,
		.name = LCM_I2C_ID_NAME,
		.of_match_table = _lcm_i2c_of_match,
	},
};

/*****************************************************************************
 * Function
 *****************************************************************************/

#ifdef VENDOR_EDIT
// shifan@bsp.tp 20191226 add for loading tp fw when screen lighting on
extern void lcd_queue_load_tp_fw(void);
#endif /*VENDOR_EDIT*/

static int _lcm_i2c_probe(struct i2c_client *client,
			  const struct i2c_device_id *id)
{
	pr_err("[LCM][I2C] %s\n", __func__);
	pr_err("[LCM][I2C] NT: info==>name=%s addr=0x%x\n", client->name,
		 client->addr);
	_lcm_i2c_client = client;
	return 0;
}

static int _lcm_i2c_remove(struct i2c_client *client)
{
	pr_err("[LCM][I2C] %s\n", __func__);
	_lcm_i2c_client = NULL;
	i2c_unregister_device(client);
	return 0;
}

static int _lcm_i2c_write_bytes(unsigned char addr, unsigned char value)
{
	int ret = 0;
	struct i2c_client *client = _lcm_i2c_client;
	char write_data[2] = { 0 };

	if (client == NULL) {
		pr_debug("ERROR!! _lcm_i2c_client is null\n");
		return 0;
	}

	write_data[0] = addr;
	write_data[1] = value;
	ret = i2c_master_send(client, write_data, 2);
	if (ret < 0)
		pr_info("[LCM][ERROR] _lcm_i2c write data fail !!\n");

	return ret;
}

/*
 * module load/unload record keeping
 */
static int __init _lcm_i2c_init(void)
{
	pr_err("[LCM][I2C] %s\n", __func__);
	i2c_add_driver(&_lcm_i2c_driver);
	pr_debug("[LCM][I2C] %s success\n", __func__);
	return 0;
}

static void __exit _lcm_i2c_exit(void)
{
	pr_err("[LCM][I2C] %s\n", __func__);
	i2c_del_driver(&_lcm_i2c_driver);
}

module_init(_lcm_i2c_init);
module_exit(_lcm_i2c_exit);
/***********************************/

struct lcm {
	struct device *dev;
	struct drm_panel panel;
	struct backlight_device *backlight;
	struct gpio_desc *reset_gpio;
	struct gpio_desc *bias_pos, *bias_neg;
	struct gpio_desc *pm_enable_gpio;

	bool prepared;
	bool enabled;

	bool hbm_en;
	bool hbm_wait;
	bool hbm_stat;           //0未在HBM  1在HBM

	int error;
};

#define lcm_dcs_write_seq(ctx, seq...) \
({\
	const u8 d[] = { seq };\
	BUILD_BUG_ON_MSG(ARRAY_SIZE(d) > 64, "DCS sequence too big for stack");\
	lcm_dcs_write(ctx, d, ARRAY_SIZE(d));\
})

#define lcm_dcs_write_seq_static(ctx, seq...) \
({\
	static const u8 d[] = { seq };\
	lcm_dcs_write(ctx, d, ARRAY_SIZE(d));\
})

static inline struct lcm *panel_to_lcm(struct drm_panel *panel)
{
	return container_of(panel, struct lcm, panel);
}

static void lcm_dcs_write(struct lcm *ctx, const void *data, size_t len)
{
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	ssize_t ret;
	char *addr;

	if (ctx->error < 0)
		return;

	addr = (char *)data;
	if ((int)*addr < 0xB0)
		ret = mipi_dsi_dcs_write_buffer(dsi, data, len);
	else
		ret = mipi_dsi_generic_write(dsi, data, len);
	if (ret < 0) {
		dev_err(ctx->dev, "error %zd writing seq: %ph\n", ret, data);
		ctx->error = ret;
	}
}

#ifdef PANEL_SUPPORT_READBACK
static int lcm_dcs_read(struct lcm *ctx, u8 cmd, void *data, size_t len)
{
	struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	ssize_t ret;

	if (ctx->error < 0)
		return 0;

	ret = mipi_dsi_dcs_read(dsi, cmd, data, len);
	if (ret < 0) {
		dev_err(ctx->dev, "error %d reading dcs seq:(%#x)\n", ret, cmd);
		ctx->error = ret;
	}

	return ret;
}

static void lcm_panel_get_data(struct lcm *ctx)
{
	u8 buffer[3] = {0};
	static int ret;

	if (ret == 0) {
		ret = lcm_dcs_read(ctx,  0x0A, buffer, 1);
		dev_info(ctx->dev, "return %d data(0x%08x) to dsi engine\n",
			 ret, buffer[0] | (buffer[1] << 8));
	}
}
#endif

#if defined(CONFIG_RT5081_PMU_DSV) || defined(CONFIG_MT6370_PMU_DSV)
static struct regulator *disp_bias_pos;
static struct regulator *disp_bias_neg;


static int lcm_panel_bias_regulator_init(void)
{
	static int regulator_inited;
	int ret = 0;

	if (regulator_inited)
		return ret;

	/* please only get regulator once in a driver */
	disp_bias_pos = regulator_get(NULL, "dsv_pos");
	if (IS_ERR(disp_bias_pos)) { /* handle return value */
		ret = PTR_ERR(disp_bias_pos);
		pr_err("get dsv_pos fail, error: %d\n", ret);
		return ret;
	}

	disp_bias_neg = regulator_get(NULL, "dsv_neg");
	if (IS_ERR(disp_bias_neg)) { /* handle return value */
		ret = PTR_ERR(disp_bias_neg);
		pr_err("get dsv_neg fail, error: %d\n", ret);
		return ret;
	}

	regulator_inited = 1;
	return ret; /* must be 0 */

}

static int lcm_panel_bias_enable(void)
{
	int ret = 0;
	int retval = 0;

	lcm_panel_bias_regulator_init();

	/* set voltage with min & max*/
	ret = regulator_set_voltage(disp_bias_pos, 5400000, 5400000);
	if (ret < 0)
		pr_err("set voltage disp_bias_pos fail, ret = %d\n", ret);
	retval |= ret;

	ret = regulator_set_voltage(disp_bias_neg, 5400000, 5400000);
	if (ret < 0)
		pr_err("set voltage disp_bias_neg fail, ret = %d\n", ret);
	retval |= ret;

	/* enable regulator */
	ret = regulator_enable(disp_bias_pos);
	if (ret < 0)
		pr_err("enable regulator disp_bias_pos fail, ret = %d\n", ret);
	retval |= ret;

	ret = regulator_enable(disp_bias_neg);
	if (ret < 0)
		pr_err("enable regulator disp_bias_neg fail, ret = %d\n", ret);
	retval |= ret;

	return retval;
}

static int lcm_panel_bias_disable(void)
{
	int ret = 0;
	int retval = 0;

	lcm_panel_bias_regulator_init();

	ret = regulator_disable(disp_bias_neg);
	if (ret < 0)
		pr_err("disable regulator disp_bias_neg fail, ret = %d\n", ret);
	retval |= ret;

	ret = regulator_disable(disp_bias_pos);
	if (ret < 0)
		pr_err("disable regulator disp_bias_pos fail, ret = %d\n", ret);
	retval |= ret;

	return retval;
}
#endif

static void lcm_panel_init(struct lcm *ctx)
{
	ctx->reset_gpio =
		devm_gpiod_get(ctx->dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset_gpio)) {
		dev_err(ctx->dev, "%s: cannot get reset_gpio %ld\n",
			__func__, PTR_ERR(ctx->reset_gpio));
		return;
	}
	char bl_last[] = {0x51,0x0F,0xFF};
	unsigned int level_normal = 125;
	level_normal = last_bl_level * 2047 / 255;
	bl_last[1] = (level_normal>>8)&0xf;
	bl_last[2] = (level_normal)&0xff;

	pr_err("gezi----------%s----%d\n",__func__,__LINE__);
	
	gpiod_set_value(ctx->reset_gpio, 0); 
	udelay(10 * 1000);
	gpiod_set_value(ctx->reset_gpio, 1);
	udelay(10 * 1000);
	gpiod_set_value(ctx->reset_gpio, 0);
	udelay(10 * 1000);
	gpiod_set_value(ctx->reset_gpio, 1);
	// udelay(250 * 1000);
	mdelay(50);
	devm_gpiod_put(ctx->dev, ctx->reset_gpio);
#if 1
//R
		lcm_dcs_write_seq_static(ctx,0xF0,0x55,0xAA,0x52,0x08,0x07);
		udelay(100);
		lcm_dcs_write_seq_static(ctx,0xC0,0x11);
		lcm_dcs_write_seq_static(ctx,0xC1,0x60,0x3C,0x02,0xA4,0xFA,0x50,0x03,0x10,0x3F,0xFD,0xFE,0x60,0x3F,0xFC,0x6B,0x20,0x00,0x00,0x60,0xAE,0x00,0x1E,0x00,0x1F,0x00,0x00,0x77,0x1E,0x00,0x00,0x00,0x00,0x1E,0x1E,0xF7,0xFF,0xFF,0xFF);
		udelay(100);
		lcm_dcs_write_seq_static(ctx,0xC2,0x48,0x3C,0x02,0x11,0xFB,0x54,0x02,0xA4,0x3F,0xFC,0xE8,0x84,0x3F,0xFE,0x1D,0x28,0x00,0x00,0x55,0x7C,0xC4,0x53,0x00,0x00,0x1F,0x00,0x1E,0x72,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xFF);
		udelay(100);
		lcm_dcs_write_seq_static(ctx,0xC3,0x4C,0x00,0x31,0x00,0x68,0x20,0x37,0x51,0x3F,0xA2,0x7B,0xE4,0x00,0x00,0xD3,0xDC,0x00,0x00,0x00,0x00,0x00,0x53,0x80,0x00,0xB1,0x90,0x79,0x23,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xFF);
		udelay(100);
		lcm_dcs_write_seq_static(ctx,0xC4,0x4A,0x3C,0x33,0xA9,0x95,0x16,0x37,0x51,0x00,0x33,0x69,0x76,0x00,0x33,0x63,0xF2,0x7F,0xDC,0x28,0xA2,0xE1,0x57,0x83,0xBE,0xAC,0x94,0x37,0x23,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xFF);
		udelay(100);
		lcm_dcs_write_seq_static(ctx,0xC5,0x46,0x00,0x37,0x51,0x70,0x7E,0x39,0x31,0x00,0x01,0xCD,0x40,0x3F,0x92,0x17,0xC0,0x00,0x00,0x00,0x00,0x00,0x1F,0x03,0xBC,0x00,0x04,0x37,0x7B,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xFF);
		udelay(100);
		lcm_dcs_write_seq_static(ctx,0xC6,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xFF);
		udelay(100);
		lcm_dcs_write_seq_static(ctx,0xC7,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xFF);
		udelay(100);
		lcm_dcs_write_seq_static(ctx,0xC8,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xFF);
		udelay(100);
		lcm_dcs_write_seq_static(ctx,0xC9,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xFF);
		udelay(100);
		lcm_dcs_write_seq_static(ctx,0xCA,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xFF);
		udelay(100);
		lcm_dcs_write_seq_static(ctx,0xCB,0x40,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xFF);
		udelay(100);
		lcm_dcs_write_seq_static(ctx,0xCC,0x40,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xFF);
		udelay(100);
		lcm_dcs_write_seq_static(ctx,0xCD,0x61,0x3C,0x05,0x10,0xF7,0x48,0x03,0xC1,0x3F,0xFE,0x0B,0xB0,0x3F,0xFE,0x37,0x48,0x00,0x00,0x2F,0xE6,0x80,0x0C,0x00,0x69,0x20,0x00,0x96,0x48,0x00,0x69,0x49,0x00,0x96,0x51,0xF7,0xFF,0xFF,0xFF);
		udelay(100);
		lcm_dcs_write_seq_static(ctx,0xCE,0x4D,0x00,0x05,0xA4,0x09,0x34,0x03,0xC1,0x3F,0xFC,0x52,0xC0,0x00,0x00,0xA4,0xB8,0x00,0x00,0x00,0x00,0x00,0x1D,0x00,0x69,0x52,0x00,0x96,0x7C,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xFF);
		udelay(100);
		lcm_dcs_write_seq_static(ctx,0xCF,0x6B,0x3C,0x05,0xA4,0xF6,0xCC,0x03,0xC1,0x00,0x01,0xB1,0x98,0x00,0x02,0x42,0xDC,0x7F,0xFF,0x87,0x2A,0x04,0x1D,0x01,0x1E,0x52,0x01,0x4B,0x7C,0x00,0x97,0x20,0x01,0x1D,0x7C,0xF7,0xFF,0xFF,0xFF);
		udelay(100);
		lcm_dcs_write_seq_static(ctx,0xD0,0x67,0x00,0x05,0x10,0x08,0xB8,0x03,0xC1,0x00,0x00,0x2C,0xB0,0x3F,0xFC,0xAE,0xF0,0x00,0x00,0x00,0x00,0x00,0x0C,0x01,0x1E,0x20,0x01,0x4B,0x48,0x01,0x1E,0x49,0x01,0x4B,0x51,0xF7,0xFF,0xFF,0xFF);
		udelay(100);
	lcm_dcs_write_seq_static(ctx,0xFF,0x55,0xA5,0x81);
	udelay(100);
	lcm_dcs_write_seq_static(ctx,0x6F,0x0D);
	lcm_dcs_write_seq_static(ctx,0xF3,0xA7);
	lcm_dcs_write_seq_static(ctx,0x6F,0x2D);
	lcm_dcs_write_seq_static(ctx,0xFA,0x80);
	lcm_dcs_write_seq_static(ctx,0x6F,0x3E);
	lcm_dcs_write_seq_static(ctx,0xFA,0x01);
	lcm_dcs_write_seq_static(ctx,0x3B,0x00,0x10,0x00,0x10);
	udelay(100);
	lcm_dcs_write_seq_static(ctx,0x03,0x01);
	lcm_dcs_write_seq_static(ctx,0x90,0x01);
	lcm_dcs_write_seq_static(ctx,0x91,0x89, 0x28, 0x00, 0x14, 0xC2, 0x00, 0x03, 0x1C, 0x02, 0x8C, 0x00, 0x0F, 0x05, 0x0E, 0x02, 0x8B, 0x10, 0xF0);
	udelay(100);
	lcm_dcs_write_seq_static(ctx,0x2F,0x04);
	lcm_dcs_write_seq_static(ctx,0x26,0x02);
	// lcm_dcs_write_seq_static(ctx,0x51,0x07,0xFF,0x0F,0xFF);
	//lcm_dcs_write_seq_static(ctx,0x51,0x07,0xFF); //comment by baibo
	lcm_dcs_write(ctx, bl_last, ARRAY_SIZE(bl_last));
	udelay(100);
	lcm_dcs_write_seq_static(ctx,0x44,0x08,0xFC);
	udelay(100);
	lcm_dcs_write_seq_static(ctx,0x53,0x20);
	lcm_dcs_write_seq_static(ctx,0x53,0x20);
	lcm_dcs_write_seq_static(ctx,0x35,0x00);
	lcm_dcs_write_seq_static(ctx,0x2A,0x00, 0x00, 0x04, 0x37);
	udelay(100);
	lcm_dcs_write_seq_static(ctx,0x2A,0x00, 0x00, 0x04, 0x37);
	udelay(100);
	lcm_dcs_write_seq_static(ctx,0x2B,0x00, 0x00, 0x09, 0x23);
	udelay(100);
#if 0
	lcm_dcs_write_seq_static(ctx,0xF0,0x55, 0xAA, 0x52, 0x08, 0x00);   //ELVSS
	udelay(100);
	lcm_dcs_write_seq_static(ctx,0xB5,0x80,0x80);
	lcm_dcs_write_seq_static(ctx,0x6F,0x07);
	lcm_dcs_write_seq_static(ctx,0xB5,0x23);
#endif
#if 1       //VGH
	lcm_dcs_write_seq_static(ctx,0xf0,0x55,0xaa,0x52,0x08,0x01);
	udelay(100);
	lcm_dcs_write_seq_static(ctx,0xB7,0x21,0x21);
#endif

	lcm_dcs_write_seq_static(ctx,0x11,0x00);
	mdelay(120);
	lcm_dcs_write_seq_static(ctx,0x29,0x00);
	//mdelay(20);
#else
	lcm_dcs_write_seq_static(ctx,0xF0,0x55, 0xAA, 0x52, 0x08, 0x00);
	udelay(100);
	lcm_dcs_write_seq_static(ctx,0x6F,0x01);
	lcm_dcs_write_seq_static(ctx,0xC0,0x33);
	lcm_dcs_write_seq_static(ctx,0x6F,0x01);
	lcm_dcs_write_seq_static(ctx,0xB5,0x00);
	lcm_dcs_write_seq_static(ctx,0x6F,0x08);
	lcm_dcs_write_seq_static(ctx,0xB5,0x4F);
	lcm_dcs_write_seq_static(ctx,0x6F,0x0D);
	lcm_dcs_write_seq_static(ctx,0xB5,0x00);
	//lcm_dcs_write_seq_static(ctx,0xBA,0x01, 0xE0, 0x00, 0x0A, 0x00, 0x0A, 0x00, 0x01);
	//lcm_dcs_write_seq_static(ctx,0xBA,0x01, 0x40, 0x00, 0x0A, 0x04, 0xA6, 0x00, 0x01);
	lcm_dcs_write_seq_static(ctx,0xF0,0x55, 0xAA, 0x52, 0x08, 0x01);
	udelay(100);
	lcm_dcs_write_seq_static(ctx,0xCD,0x08, 0xC1);
	udelay(100);
	lcm_dcs_write_seq_static(ctx,0xD2,0x00, 0x40, 0x11);
	udelay(100);
	lcm_dcs_write_seq_static(ctx,0xF0,0x55, 0xAA, 0x52, 0x08, 0x05);
	udelay(100);
	lcm_dcs_write_seq_static(ctx,0xB3,0x82, 0x80);
	udelay(100);
	lcm_dcs_write_seq_static(ctx,0xB5,0x86, 0x81, 0x00, 0x00);
	udelay(100);
	lcm_dcs_write_seq_static(ctx,0xB7,0x05, 0x00, 0x00, 0x81);
	udelay(100);
	lcm_dcs_write_seq_static(ctx,0xB8,0x06, 0x00, 0x00, 0x81);
	udelay(100);
	lcm_dcs_write_seq_static(ctx,0xF0,0x55, 0xAA, 0x52, 0x08, 0x07);
	udelay(100);
	lcm_dcs_write_seq_static(ctx,0xC0,0x00);
	lcm_dcs_write_seq_static(ctx,0x6F,0x04);
	lcm_dcs_write_seq_static(ctx,0xB0,0x00);
	lcm_dcs_write_seq_static(ctx,0xFF,0xAA, 0x55, 0xA5, 0x81);
	udelay(100);
	lcm_dcs_write_seq_static(ctx,0x6F,0x0D);
	lcm_dcs_write_seq_static(ctx,0xF3,0xA7);
	lcm_dcs_write_seq_static(ctx,0x6F,0x2D);
	lcm_dcs_write_seq_static(ctx,0xFA,0x80);
	lcm_dcs_write_seq_static(ctx,0x6F,0x3E);
	lcm_dcs_write_seq_static(ctx,0xFA,0x01);
	lcm_dcs_write_seq_static(ctx,0x03,0x00);
	lcm_dcs_write_seq_static(ctx,0x90,0x02);
	lcm_dcs_write_seq_static(ctx,0x91,0x89, 0x28, 0x00, 0x14, 0xC2, 0x00, 0x03, 0x1C, 0x02, 0x8C, 0x00, 0x0F, 0x05, 0x0E, 0x02, 0x8B, 0x10, 0xF0);
	udelay(100);
	lcm_dcs_write_seq_static(ctx,0x44,0x08, 0xFC);
	udelay(100);
	lcm_dcs_write_seq_static(ctx,0x35,0x00);
	lcm_dcs_write_seq_static(ctx,0x35,0x00);
	lcm_dcs_write_seq_static(ctx,0x53,0x20);
	lcm_dcs_write_seq_static(ctx,0x53,0x20);
	lcm_dcs_write_seq_static(ctx,0x2F,0x01);
	lcm_dcs_write_seq_static(ctx,0x26,0x01);
	lcm_dcs_write_seq_static(ctx,0x2A,0x00, 0x00, 0x04, 0x37);
	udelay(100);
	lcm_dcs_write_seq_static(ctx,0x2B,0x00, 0x00, 0x09, 0x23);
	udelay(100);
	lcm_dcs_write_seq_static(ctx,0x3B,0x00, 0x10, 0x00, 0x10);
	udelay(100);
	#if 0
	lcm_dcs_write_seq_static(ctx,0xF0,0x55, 0xAA, 0x52, 0x08, 0x00);   //bist mode
	lcm_dcs_write_seq_static(ctx,0xED,0x01);
	lcm_dcs_write_seq_static(ctx,0xEE,0x87, 0x78, 0x02, 0x40);
	lcm_dcs_write_seq_static(ctx,0xEF,0x01, 0x00, 0xFF, 0xFF, 0xFF, 0x47, 0xFF, 0x05,0xC0);
	#endif
	#if 1
	//lcm_dcs_write_seq_static(ctx,0x51,0x00,0xFF);
	//udelay(100);
	#endif
	// lcm_dcs_write_seq_static(ctx,0xF0,0x55, 0xAA, 0x52, 0x08, 0x00);
	// lcm_dcs_write_seq_static(ctx,0x6F,0x06);
	// lcm_dcs_write_seq_static(ctx,0xB5,0x2D);
	lcm_dcs_write_seq_static(ctx,0x11,0x00);
	mdelay(120);
	lcm_dcs_write_seq_static(ctx,0x29,0x00);
	mdelay(79);
#endif
}

static int lcm_disable(struct drm_panel *panel)
{
	struct lcm *ctx = panel_to_lcm(panel);

	if (!ctx->enabled)
		return 0;
	pr_err("gezi----exit------%s-----%d\n",__func__,__LINE__);
	if (ctx->backlight) {
		ctx->backlight->props.power = FB_BLANK_POWERDOWN;
		backlight_update_status(ctx->backlight);
	}

	ctx->enabled = false;

	return 0;
}

static int lcm_unprepare(struct drm_panel *panel)
{
	struct lcm *ctx = panel_to_lcm(panel);
	
	pr_err("gezi----------%s-----%d\n",__func__,__LINE__);

	if (!ctx->prepared)
		return 0;

	lcm_dcs_write_seq_static(ctx, 0x28);
	lcm_dcs_write_seq_static(ctx, 0x10);
	msleep(120);

	ctx->error = 0;
	ctx->prepared = false;
#if defined(CONFIG_RT5081_PMU_DSV) || defined(CONFIG_MT6370_PMU_DSV)
	lcm_panel_bias_disable();
	pr_err("gezi----------%s-----%d\n",__func__,__LINE__);
#else
	pr_err("gezi------exit----%s-----%d\n",__func__,__LINE__);
	//reset 
	ctx->reset_gpio =
		devm_gpiod_get(ctx->dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset_gpio)) {
		dev_err(ctx->dev, "%s: cannot get reset_gpio %ld\n",
			__func__, PTR_ERR(ctx->reset_gpio));
		return PTR_ERR(ctx->reset_gpio);
	}
	gpiod_set_value(ctx->reset_gpio, 0);
	devm_gpiod_put(ctx->dev, ctx->reset_gpio);

	// 138   --  2.8
	ctx->bias_neg = devm_gpiod_get_index(ctx->dev,
		"bias", 1, GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->bias_neg)) {
		dev_err(ctx->dev, "%s: cannot get bias_neg %ld\n",
			__func__, PTR_ERR(ctx->bias_neg));
		return PTR_ERR(ctx->bias_neg);
	}
	gpiod_set_value(ctx->bias_neg, 0);
	devm_gpiod_put(ctx->dev, ctx->bias_neg);

	udelay(1000);
	
	pr_err("gezi----------%s-----%d\n",__func__,__LINE__);

	//137  -  1.2
	ctx->bias_pos = devm_gpiod_get_index(ctx->dev,
		"bias", 0, GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->bias_pos)) {
		dev_err(ctx->dev, "%s: cannot get bias_pos %ld\n",
			__func__, PTR_ERR(ctx->bias_pos));
		return PTR_ERR(ctx->bias_pos);
	}
	gpiod_set_value(ctx->bias_pos, 0);
	devm_gpiod_put(ctx->dev, ctx->bias_pos);

	udelay(2000);	


	//prize add by wangfei for ldo 1.8 20210709 start
	ctx->pm_enable_gpio = devm_gpiod_get_index(ctx->dev,
		"pm-enable", 0, GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->pm_enable_gpio)) {
		dev_err(ctx->dev, "%s: cannot get pm_enable_gpio %ld\n",
			__func__, PTR_ERR(ctx->pm_enable_gpio));
		return PTR_ERR(ctx->pm_enable_gpio);
	}
	gpiod_set_value(ctx->pm_enable_gpio, 0);
	devm_gpiod_put(ctx->dev, ctx->pm_enable_gpio);
	//prize add by wangfei for ldo 1.8 20210709 end
	
#endif
//prize add by xuejian, for set unprepare backlight hbm_stat 20220406 start 
       g_ctx->hbm_stat = false;
//prize add by xuejian, for set unprepare backlight hbm_stat 20220406 end
	ctx->hbm_en = false;
	return 0;
}

static int lcm_prepare(struct drm_panel *panel)
{
	struct lcm *ctx = panel_to_lcm(panel);
	int ret;

	pr_info("%s\n", __func__);
	
	pr_err("gezi----------%s-----%d\n",__func__,__LINE__);
	
	if (ctx->prepared)
		return 0;

#if defined(CONFIG_RT5081_PMU_DSV) || defined(CONFIG_MT6370_PMU_DSV)
	lcm_panel_bias_enable();
	pr_err("gezi----------%s-----%d\n",__func__,__LINE__);
#else

	//prize add by wangfei for ldo 1.8 20210709 start
	ctx->pm_enable_gpio = devm_gpiod_get_index(ctx->dev,
		"pm-enable", 0, GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->pm_enable_gpio)) {
		dev_err(ctx->dev, "%s: cannot get pm_enable_gpio %ld\n",
			__func__, PTR_ERR(ctx->pm_enable_gpio));
		return PTR_ERR(ctx->pm_enable_gpio);
	}
	gpiod_set_value(ctx->pm_enable_gpio, 1);
	devm_gpiod_put(ctx->dev, ctx->pm_enable_gpio);
	//prize add by wangfei for ldo 1.8 20210709 end
	udelay(2000);	


	ctx->bias_pos = devm_gpiod_get_index(ctx->dev,
		"bias", 0, GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->bias_pos)) {
		dev_err(ctx->dev, "%s: cannot get bias_pos %ld\n",
			__func__, PTR_ERR(ctx->bias_pos));
		return PTR_ERR(ctx->bias_pos);
	}
	gpiod_set_value(ctx->bias_pos, 1);
	devm_gpiod_put(ctx->dev, ctx->bias_pos);

	udelay(2000);
	
	pr_err("gezi----------%s-----%d\n",__func__,__LINE__);

	ctx->bias_neg = devm_gpiod_get_index(ctx->dev,
		"bias", 1, GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->bias_neg)) {
		dev_err(ctx->dev, "%s: cannot get bias_neg %ld\n",
			__func__, PTR_ERR(ctx->bias_neg));
		return PTR_ERR(ctx->bias_neg);
	}
	gpiod_set_value(ctx->bias_neg, 1);
	devm_gpiod_put(ctx->dev, ctx->bias_neg);
	
	// _lcm_i2c_write_bytes(0x0, 0xf);
	// _lcm_i2c_write_bytes(0x1, 0xf);
	
	pr_err("gezi----------%s-----%d\n",__func__,__LINE__);
#endif


	lcm_panel_init(ctx);

	ret = ctx->error;
	if (ret < 0)
		lcm_unprepare(panel);

	ctx->prepared = true;
	
	pr_err("gezi----------%s-----%d\n",__func__,__LINE__);

#if defined(CONFIG_MTK_PANEL_EXT)
	mtk_panel_tch_rst(panel);
	pr_err("gezi----------%s-----%d\n",__func__,__LINE__);
#endif
#ifdef PANEL_SUPPORT_READBACK
	lcm_panel_get_data(ctx);
	pr_err("gezi----------%s-----%d\n",__func__,__LINE__);
#endif
	pr_err("gezi----exit------%s-----%d\n",__func__,__LINE__);
	return ret;
}

static int lcm_enable(struct drm_panel *panel)
{
	struct lcm *ctx = panel_to_lcm(panel);

	if (ctx->enabled)
		return 0;

	if (ctx->backlight) {
		ctx->backlight->props.power = FB_BLANK_UNBLANK;
		backlight_update_status(ctx->backlight);
	}
	pr_err("gezi----exit------%s-----%d\n",__func__,__LINE__);
	ctx->enabled = true;

	return 0;
} 

// #define HFP (100)
// #define HSA (8)
// #define HBP (92)
// #define VFP (72)
// #define VSA (4)
// #define VBP (8)
#define VAC (2340)
#define HAC (1080)
static u32 fake_heigh = 2340;
static u32 fake_width = 1080;
static bool need_fake_resolution;

// static struct drm_display_mode default_mode = {
// 	.clock = 266090,//htotal*vtotal*vrefresh/1000
// 	.hdisplay = HAC,
// 	.hsync_start = HAC + HFP,
// 	.hsync_end = HAC + HFP + HSA,
// 	.htotal = HAC + HFP + HSA + HBP,
// 	.vdisplay = VAC,
// 	.vsync_start = VAC + VFP,
// 	.vsync_end = VAC + VFP + VSA,
// 	.vtotal = VAC + VFP + VSA + VBP,
// 	.vrefresh = 90,
// };

static const struct drm_display_mode switch_mode_90 = {
	.clock = 266090,
	.hdisplay = FRAME_WIDTH,
	.hsync_start = FRAME_WIDTH + MODE_1_HFP,
	.hsync_end = FRAME_WIDTH + MODE_1_HFP + HSA,
	.htotal = FRAME_WIDTH + MODE_1_HFP + HSA + HBP,
	.vdisplay = FRAME_HEIGHT,
	.vsync_start = FRAME_HEIGHT + MODE_1_VFP,
	.vsync_end = FRAME_HEIGHT + MODE_1_VFP + VSA,
	.vtotal = FRAME_HEIGHT + MODE_1_VFP + VSA + VBP,
	.vrefresh = MODE_1_FPS,
};

static const struct drm_display_mode default_mode = {
	.clock = 227090,
	.hdisplay = FRAME_WIDTH,
	.hsync_start = FRAME_WIDTH + MODE_0_HFP,
	.hsync_end = FRAME_WIDTH + MODE_0_HFP + HSA,
	.htotal = FRAME_WIDTH + MODE_0_HFP + HSA + HBP,
	.vdisplay = FRAME_HEIGHT,
	.vsync_start = FRAME_HEIGHT + MODE_0_VFP,
	.vsync_end = FRAME_HEIGHT + MODE_0_VFP + VSA,
	.vtotal = FRAME_HEIGHT + MODE_0_VFP + VSA + VBP,
	.vrefresh = MODE_0_FPS,
};

#if defined(CONFIG_MTK_PANEL_EXT)
static int panel_ext_reset(struct drm_panel *panel, int on)
{
	struct lcm *ctx = panel_to_lcm(panel);
	pr_err("wolf----exit------%s-----%d\n",__func__,__LINE__);
	ctx->reset_gpio =
		devm_gpiod_get(ctx->dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset_gpio)) {
		dev_err(ctx->dev, "%s: cannot get reset_gpio %ld\n",
			__func__, PTR_ERR(ctx->reset_gpio));
		return PTR_ERR(ctx->reset_gpio);
	}
	gpiod_set_value(ctx->reset_gpio, on);
	devm_gpiod_put(ctx->dev, ctx->reset_gpio);

	return 0;
}

static int panel_ata_check(struct drm_panel *panel)
{
	struct lcm *ctx = panel_to_lcm(panel);
	struct gpio_desc *id2_gpio = NULL;
	//struct mipi_dsi_device *dsi = to_mipi_dsi_device(ctx->dev);
	//unsigned char data[3] = {0x00, 0x00, 0x00};
	//unsigned char id[3] = {0x0, 0x81, 0x0};
	ssize_t ret;
	pr_err("wolf----exit------%s-----%d\n",__func__,__LINE__);

	// prize baibo for lcm ata test begin
#if 0
	ret = mipi_dsi_dcs_read(dsi, 0x4, data, 3);
	if (ret < 0) {
		pr_err("%s error\n", __func__);
		return 0;
	}

	DDPINFO("ATA read data %x %x %x\n", data[0], data[1], data[2]);

	if (data[0] == id[0] &&
			data[1] == id[1] &&
			data[2] == id[2])
		return 1;

	DDPINFO("ATA expect read data is %x %x %x\n",
			id[0], id[1], id[2]);
#endif

	id2_gpio = devm_gpiod_get(ctx->dev, "id2", GPIOD_IN);
	if (IS_ERR(id2_gpio)) {
		dev_err(ctx->dev, "%s: cannot get id2_gpio %ld\n",
			__func__, PTR_ERR(id2_gpio));
		return 0;
	}

	if (gpiod_get_value(id2_gpio)) {
		pr_err("%s %d id2 value 1\n",__func__,__LINE__);
		ret = 0;
	}else{
		pr_err("%s %d id2 value 0\n",__func__,__LINE__);
		ret = 1;
	}
	devm_gpiod_put(ctx->dev, id2_gpio);

	return ret;
	// prize baibo for lcm ata test end
}

static int lcm_setbacklight_cmdq(void *dsi, dcs_write_gce cb,
	void *handle, unsigned int level)
{
	 char bl_tb0[] = {0x51,0x07,0xFF};
	 char hbm_tb[] = {0x51,0x0F,0xFF};
	 unsigned int level_normal = 125;
	// printk("wolf %s level = %d\n",__func__,level);
	 if(level > 255)
	 {
	 	if(level == 260)     //进入HBM
	 	{
	 		printk("wolf into HBM\n");
		//prize del by wangfei for don't set ELVSS when into HBM 20211102 start
	 	//	lcm_dcs_write_seq_static(g_ctx,0xF0,0x55, 0xAA, 0x52, 0x08, 0x00);   //ELVSS
		//	udelay(100);
		//	lcm_dcs_write_seq_static(g_ctx,0xB5,0x80,0x80);
		//	lcm_dcs_write_seq_static(g_ctx,0x6F,0x07);
		//	lcm_dcs_write_seq_static(g_ctx,0xB5,0x1D);
		//prize del by wangfei for don't set ELVSS when into HBM 20211102 end
			if (!cb)
				return -1;
			g_ctx->hbm_stat = true;
			cb(dsi, handle, hbm_tb, ARRAY_SIZE(hbm_tb));
	 	}
	 	else if(level == 270)  //退出HBM
	 	{
	 		level_normal = bl_level * 2047 / 255;
			bl_tb0[1] = (level_normal>>8)&0xf;
			bl_tb0[2] = (level_normal)&0xff;
			if (!cb)
				return -1;
			printk("wolf out HBM bl_level = %d\n",bl_level);
			g_ctx->hbm_stat = false;
			cb(dsi, handle, bl_tb0, ARRAY_SIZE(bl_tb0));
	 	}
	 }
	 else 
	 {
		level = level * 2047 / 255;
		bl_tb0[1] = (level>>8)&0xf;
		bl_tb0[2] = (level)&0xff;
		pr_err("wolf ---------%d----level = %d----,bl_tb0[1] = %d,bl_tb0[2] = %d\n",__LINE__,level/8,bl_tb0[1],bl_tb0[2]);
		if (!cb)
			return -1;
		if(g_ctx->hbm_stat == false)
			cb(dsi, handle, bl_tb0, ARRAY_SIZE(bl_tb0));
		bl_level = level/8;
		if(bl_level > 0)
			last_bl_level = bl_level;
	 }
	 return 0;
}

static int panel_hbm_set_cmdq(struct drm_panel *panel, void *dsi,
			      dcs_write_gce cb, void *handle, bool en)
{
	unsigned int level_hbm = 255;
	unsigned int level_normal = 125;
	char normal_tb0[] = {0x51, 0x07,0xFF};
	char hbm_tb[] = {0x51,0x0F,0xFF};
	struct lcm *ctx = panel_to_lcm(panel);

	if (!cb)
		return -1;

	//if (ctx->hbm_en == en)
	//	goto done;

	printk("[wolf] %s : en = %d\n",__func__,en);
	if (en)
	{
		printk("[wolf] %s : set HBM\n",__func__);
		// level_hbm = level_hbm*16;
		// hbm_tb[1] = (level_hbm>>8)&0xff;
		// hbm_tb[2] = (level_hbm)&0xff;
	#if 1
		lcm_dcs_write_seq_static(ctx,0xF0,0x55, 0xAA, 0x52, 0x08, 0x00);   //ELVSS
		udelay(100);
		lcm_dcs_write_seq_static(ctx,0xB5,0x80,0x80);
		lcm_dcs_write_seq_static(ctx,0x6F,0x07);
		lcm_dcs_write_seq_static(ctx,0xB5,0x1D);
	#endif
		cb(dsi, handle, hbm_tb, ARRAY_SIZE(hbm_tb));
	}
	else
	{
		printk("[wolf] %s : set normal = %d\n",__func__,bl_level);
		 level_normal = bl_level* 2047 / 255;
		 normal_tb0[1] = (level_normal>>8)&0xff;
		 normal_tb0[2] = (level_normal)&0xff;
	#if 1
		lcm_dcs_write_seq_static(ctx,0xF0,0x55, 0xAA, 0x52, 0x08, 0x00);   //ELVSS
		udelay(100);
		lcm_dcs_write_seq_static(ctx,0xB5,0x80,0x80);
		lcm_dcs_write_seq_static(ctx,0x6F,0x07);
		lcm_dcs_write_seq_static(ctx,0xB5,0x23);
	#endif
		cb(dsi, handle, normal_tb0, ARRAY_SIZE(normal_tb0));
	}

	

	ctx->hbm_en = en;
	ctx->hbm_wait = true;

 done:
	return 0;
}

static void panel_hbm_get_state(struct drm_panel *panel, bool *state)
{
	struct lcm *ctx = panel_to_lcm(panel);

	*state = ctx->hbm_en;
}

static int lcm_get_virtual_heigh(void)
{
	return VAC;
}

static int lcm_get_virtual_width(void)
{
	return HAC;
}

static struct mtk_panel_params ext_params_90 = {
	// .vfp_low_power = 743,
	.cust_esd_check = 0,
	.esd_check_enable = 0,
	.lcm_esd_check_table[0] = {
		.cmd = 0x53,
		.count = 1,
		.para_list[0] = 0x24,
	},
	.physical_width_um = PHYSICAL_WIDTH,
	.physical_height_um = PHYSICAL_HEIGHT,
	.dsc_params = {
			.bdg_dsc_enable = 1,
			.ver                   =  DSC_VER,
			.slice_mode            =  DSC_SLICE_MODE,
			.rgb_swap              =  DSC_RGB_SWAP,
			.dsc_cfg               =  DSC_DSC_CFG,
			.rct_on                =  DSC_RCT_ON,
			.bit_per_channel       =  DSC_BIT_PER_CHANNEL,
			.dsc_line_buf_depth    =  DSC_DSC_LINE_BUF_DEPTH,
			.bp_enable             =  DSC_BP_ENABLE,
			.bit_per_pixel         =  DSC_BIT_PER_PIXEL,
			.pic_height            =  FRAME_HEIGHT,
			.pic_width             =  FRAME_WIDTH,
			.slice_height          =  DSC_SLICE_HEIGHT,
			.slice_width           =  DSC_SLICE_WIDTH,
			.chunk_size            =  DSC_CHUNK_SIZE,
			.xmit_delay            =  DSC_XMIT_DELAY, 
			.dec_delay             =  DSC_DEC_DELAY,
			.scale_value           =  DSC_SCALE_VALUE,
			.increment_interval    =  DSC_INCREMENT_INTERVAL,
			.decrement_interval    =  DSC_DECREMENT_INTERVAL,
			.line_bpg_offset       =  DSC_LINE_BPG_OFFSET,
			.nfl_bpg_offset        =  DSC_NFL_BPG_OFFSET,
			.slice_bpg_offset      =  DSC_SLICE_BPG_OFFSET,
			.initial_offset        =  DSC_INITIAL_OFFSET,
			.final_offset          =  DSC_FINAL_OFFSET,
			.flatness_minqp        =  DSC_FLATNESS_MINQP,
			.flatness_maxqp        =  DSC_FLATNESS_MAXQP,
			.rc_model_size         =  DSC_RC_MODEL_SIZE,
			.rc_edge_factor        =  DSC_RC_EDGE_FACTOR,
			.rc_quant_incr_limit0  =  DSC_RC_QUANT_INCR_LIMIT0,
			.rc_quant_incr_limit1  =  DSC_RC_QUANT_INCR_LIMIT1,
			.rc_tgt_offset_hi      =  DSC_RC_TGT_OFFSET_HI,
			.rc_tgt_offset_lo      =  DSC_RC_TGT_OFFSET_LO,
		},
//	.wait_sof_before_dec_vfp = 1,

	.dyn_fps = {
		.switch_en = 1,
	},
	.data_rate = MODE_1_DATA_RATE,
	.bdg_ssc_disable = 1,
	.ssc_disable = 1,
	.dyn = {
		.switch_en = 1,
		.pll_clk = 250,
	},
#ifdef CONFIG_MTK_ROUND_CORNER_SUPPORT
	.round_corner_en = 1,
	.corner_pattern_height = ROUND_CORNER_H_TOP,
	.corner_pattern_height_bot = ROUND_CORNER_H_BOT,
	.corner_pattern_tp_size = sizeof(top_rc_pattern),
	.corner_pattern_lt_addr = (void *)top_rc_pattern,
#endif
};


static struct mtk_panel_params ext_params = {
	// .pll_clk = 373,
	// .vfp_low_power = 743,
	.cust_esd_check = 0,
	.esd_check_enable = 0,
	.lcm_esd_check_table[0] = {
		.cmd = 0x53,
		.count = 1,
		.para_list[0] = 0x24,
	},
	.physical_width_um = PHYSICAL_WIDTH,
	.physical_height_um = PHYSICAL_HEIGHT,
	.dsc_params = {
			.bdg_dsc_enable = 1,
			.ver                   =  DSC_VER,
			.slice_mode            =  DSC_SLICE_MODE,
			.rgb_swap              =  DSC_RGB_SWAP,
			.dsc_cfg               =  DSC_DSC_CFG,
			.rct_on                =  DSC_RCT_ON,
			.bit_per_channel       =  DSC_BIT_PER_CHANNEL,
			.dsc_line_buf_depth    =  DSC_DSC_LINE_BUF_DEPTH,
			.bp_enable             =  DSC_BP_ENABLE,
			.bit_per_pixel         =  DSC_BIT_PER_PIXEL,
			.pic_height            =  FRAME_HEIGHT,
			.pic_width             =  FRAME_WIDTH,
			.slice_height          =  DSC_SLICE_HEIGHT,
			.slice_width           =  DSC_SLICE_WIDTH,
			.chunk_size            =  DSC_CHUNK_SIZE,
			.xmit_delay            =  DSC_XMIT_DELAY,
			.dec_delay             =  DSC_DEC_DELAY,
			.scale_value           =  DSC_SCALE_VALUE,
			.increment_interval    =  DSC_INCREMENT_INTERVAL,
			.decrement_interval    =  DSC_DECREMENT_INTERVAL,
			.line_bpg_offset       =  DSC_LINE_BPG_OFFSET,
			.nfl_bpg_offset        =  DSC_NFL_BPG_OFFSET,
			.slice_bpg_offset      =  DSC_SLICE_BPG_OFFSET,
			.initial_offset        =  DSC_INITIAL_OFFSET,
			.final_offset          =  DSC_FINAL_OFFSET,
			.flatness_minqp        =  DSC_FLATNESS_MINQP,
			.flatness_maxqp        =  DSC_FLATNESS_MAXQP,
			.rc_model_size         =  DSC_RC_MODEL_SIZE,
			.rc_edge_factor        =  DSC_RC_EDGE_FACTOR,
			.rc_quant_incr_limit0  =  DSC_RC_QUANT_INCR_LIMIT0,
			.rc_quant_incr_limit1  =  DSC_RC_QUANT_INCR_LIMIT1,
			.rc_tgt_offset_hi      =  DSC_RC_TGT_OFFSET_HI,
			.rc_tgt_offset_lo      =  DSC_RC_TGT_OFFSET_LO,
		},
//	.wait_sof_before_dec_vfp = 1,
	.data_rate = MODE_1_DATA_RATE,
	.bdg_ssc_disable = 1,
	.ssc_disable = 1,
	.dyn_fps = {
		.switch_en = 1,
	},
	.dyn = {
		.switch_en = 1,
		.pll_clk = 250,
	},
#ifdef CONFIG_MTK_ROUND_CORNER_SUPPORT
	.round_corner_en = 1,
	.corner_pattern_height = ROUND_CORNER_H_TOP,
	.corner_pattern_height_bot = ROUND_CORNER_H_BOT,
	.corner_pattern_tp_size = sizeof(top_rc_pattern),
	.corner_pattern_lt_addr = (void *)top_rc_pattern,
#endif
};


struct drm_display_mode *get_mode_by_id(struct drm_panel *panel,
	unsigned int mode)
{
	struct drm_display_mode *m;
	unsigned int i = 0;

	list_for_each_entry(m, &panel->connector->modes, head) {
		if (i == mode)
			return m;
		i++;
	}
	return NULL;
}

static int mtk_panel_ext_param_set(struct drm_panel *panel,
			 unsigned int mode)
{
	struct mtk_panel_ext *ext = find_panel_ext(panel);
	int ret = 0;
	struct drm_display_mode *m = get_mode_by_id(panel, mode);
	printk("[wolf] %s,vrefresh = %d\n",__func__,m->vrefresh);
	if (m->vrefresh == MODE_0_FPS)
		ext->params = &ext_params;
	else if (m->vrefresh == MODE_1_FPS)
		ext->params = &ext_params_90;
	else
		ret = 1;

	return ret;
}

static void mode_switch_to_90(struct drm_panel *panel,
	enum MTK_PANEL_MODE_SWITCH_STAGE stage)
{
	struct lcm *ctx = panel_to_lcm(panel);

	if (stage == BEFORE_DSI_POWERDOWN) {
		printk("[wolf] %s\n",__func__);
		lcm_dcs_write_seq_static(ctx,0xF0,0x55,0xAA,0x52,0x08,0x07);
		udelay(100);
		lcm_dcs_write_seq_static(ctx,0xC0,0x11);
		lcm_dcs_write_seq_static(ctx,0xC1,0x60,0x3C,0x02,0xA4,0xFA,0x50,0x03,0x10,0x3F,0xFD,0xFE,0x60,0x3F,0xFC,0x6B,0x20,0x00,0x00,0x60,0xAE,0x00,0x1E,0x00,0x1F,0x00,0x00,0x77,0x1E,0x00,0x00,0x00,0x00,0x1E,0x1E,0xF7,0xFF,0xFF,0xFF);
		udelay(100);
		lcm_dcs_write_seq_static(ctx,0xC2,0x48,0x3C,0x02,0x11,0xFB,0x54,0x02,0xA4,0x3F,0xFC,0xE8,0x84,0x3F,0xFE,0x1D,0x28,0x00,0x00,0x55,0x7C,0xC4,0x53,0x00,0x00,0x1F,0x00,0x1E,0x72,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xFF);
		udelay(100);
		lcm_dcs_write_seq_static(ctx,0xC3,0x4C,0x00,0x31,0x00,0x68,0x20,0x37,0x51,0x3F,0xA2,0x7B,0xE4,0x00,0x00,0xD3,0xDC,0x00,0x00,0x00,0x00,0x00,0x53,0x80,0x00,0xB1,0x90,0x79,0x23,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xFF);
		udelay(100);
		lcm_dcs_write_seq_static(ctx,0xC4,0x4A,0x3C,0x33,0xA9,0x95,0x16,0x37,0x51,0x00,0x33,0x69,0x76,0x00,0x33,0x63,0xF2,0x7F,0xDC,0x28,0xA2,0xE1,0x57,0x83,0xBE,0xAC,0x94,0x37,0x23,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xFF);
		udelay(100);
		lcm_dcs_write_seq_static(ctx,0xC5,0x46,0x00,0x37,0x51,0x70,0x7E,0x39,0x31,0x00,0x01,0xCD,0x40,0x3F,0x92,0x17,0xC0,0x00,0x00,0x00,0x00,0x00,0x1F,0x03,0xBC,0x00,0x04,0x37,0x7B,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xFF);
		udelay(100);
		lcm_dcs_write_seq_static(ctx,0xC6,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xFF);
		udelay(100);
		lcm_dcs_write_seq_static(ctx,0xC7,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xFF);
		udelay(100);
		lcm_dcs_write_seq_static(ctx,0xC8,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xFF);
		udelay(100);
		lcm_dcs_write_seq_static(ctx,0xC9,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xFF);
		udelay(100);
		lcm_dcs_write_seq_static(ctx,0xCA,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xFF);
		udelay(100);
		lcm_dcs_write_seq_static(ctx,0xCB,0x40,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xFF);
		udelay(100);
		lcm_dcs_write_seq_static(ctx,0xCC,0x40,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xFF);
		udelay(100);
		lcm_dcs_write_seq_static(ctx,0xCD,0x61,0x3C,0x05,0x10,0xF7,0x48,0x03,0xC1,0x3F,0xFE,0x0B,0xB0,0x3F,0xFE,0x37,0x48,0x00,0x00,0x2F,0xE6,0x80,0x0C,0x00,0x69,0x20,0x00,0x96,0x48,0x00,0x69,0x49,0x00,0x96,0x51,0xF7,0xFF,0xFF,0xFF);
		udelay(100);
		lcm_dcs_write_seq_static(ctx,0xCE,0x4D,0x00,0x05,0xA4,0x09,0x34,0x03,0xC1,0x3F,0xFC,0x52,0xC0,0x00,0x00,0xA4,0xB8,0x00,0x00,0x00,0x00,0x00,0x1D,0x00,0x69,0x52,0x00,0x96,0x7C,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xFF);
		udelay(100);
		lcm_dcs_write_seq_static(ctx,0xCF,0x6B,0x3C,0x05,0xA4,0xF6,0xCC,0x03,0xC1,0x00,0x01,0xB1,0x98,0x00,0x02,0x42,0xDC,0x7F,0xFF,0x87,0x2A,0x04,0x1D,0x01,0x1E,0x52,0x01,0x4B,0x7C,0x00,0x97,0x20,0x01,0x1D,0x7C,0xF7,0xFF,0xFF,0xFF);
		udelay(100);
		lcm_dcs_write_seq_static(ctx,0xD0,0x67,0x00,0x05,0x10,0x08,0xB8,0x03,0xC1,0x00,0x00,0x2C,0xB0,0x3F,0xFC,0xAE,0xF0,0x00,0x00,0x00,0x00,0x00,0x0C,0x01,0x1E,0x20,0x01,0x4B,0x48,0x01,0x1E,0x49,0x01,0x4B,0x51,0xF7,0xFF,0xFF,0xFF);
		udelay(100);
		lcm_dcs_write_seq_static(ctx,0xFF,0x55,0xA5,0x81);
		udelay(100);
		lcm_dcs_write_seq_static(ctx,0x6F,0x0D);
		lcm_dcs_write_seq_static(ctx,0xF3,0xA7);
		lcm_dcs_write_seq_static(ctx,0x6F,0x2D);
		lcm_dcs_write_seq_static(ctx,0xFA,0x80);
		lcm_dcs_write_seq_static(ctx,0x6F,0x3E);
		lcm_dcs_write_seq_static(ctx,0xFA,0x01);
		lcm_dcs_write_seq_static(ctx,0x3B,0x00,0x10,0x00,0x10);
		udelay(100);
		lcm_dcs_write_seq_static(ctx,0x03,0x01);
		lcm_dcs_write_seq_static(ctx,0x90,0x01);
		lcm_dcs_write_seq_static(ctx,0x91,0x89, 0x28, 0x00, 0x14, 0xC2, 0x00, 0x03, 0x1C, 0x02, 0x8C, 0x00, 0x0F, 0x05, 0x0E, 0x02, 0x8B, 0x10, 0xF0);
		udelay(100);
		lcm_dcs_write_seq_static(ctx,0x2F,0x04);
		lcm_dcs_write_seq_static(ctx,0x26,0x02);
		// lcm_dcs_write_seq_static(ctx,0x51,0x07,0xFF,0x0F,0xFF);
		// lcm_dcs_write_seq_static(ctx,0x51,0x07,0xFF);
		udelay(100);
		lcm_dcs_write_seq_static(ctx,0x44,0x08,0xFC);
		udelay(100);
		lcm_dcs_write_seq_static(ctx,0x53,0x20);
		lcm_dcs_write_seq_static(ctx,0x53,0x20); 
		lcm_dcs_write_seq_static(ctx,0x35,0x00);
		lcm_dcs_write_seq_static(ctx,0x2A,0x00, 0x00, 0x04, 0x37);
		udelay(100);
		lcm_dcs_write_seq_static(ctx,0x2A,0x00, 0x00, 0x04, 0x37);
		udelay(100);
		lcm_dcs_write_seq_static(ctx,0x2B,0x00, 0x00, 0x09, 0x23);
		udelay(100);
	#if 0
		lcm_dcs_write_seq_static(ctx,0xF0,0x55, 0xAA, 0x52, 0x08, 0x00);   //ELVSS
		udelay(100);
		lcm_dcs_write_seq_static(ctx,0xB5,0x80,0x80);
		lcm_dcs_write_seq_static(ctx,0x6F,0x07);
		lcm_dcs_write_seq_static(ctx,0xB5,0x23);
	#endif
	#if 1       //VGH
	lcm_dcs_write_seq_static(ctx,0xf0,0x55,0xaa,0x52,0x08,0x01);
	udelay(100);
	lcm_dcs_write_seq_static(ctx,0xB7,0x21,0x21);
	#endif
	}
}

static void mode_switch_to_60(struct drm_panel *panel,
	enum MTK_PANEL_MODE_SWITCH_STAGE stage)
{
	struct lcm *ctx = panel_to_lcm(panel);

	if (stage == BEFORE_DSI_POWERDOWN) {
		printk("[wolf] %s\n",__func__);
		lcm_dcs_write_seq_static(ctx,0xF0,0x55,0xAA,0x52,0x08,0x07);
		udelay(100);
		lcm_dcs_write_seq_static(ctx,0xC0,0x11);
		lcm_dcs_write_seq_static(ctx,0xC1,0x60,0x3C,0x02,0xA4,0xFA,0x50,0x03,0x10,0x3F,0xFD,0xFE,0x60,0x3F,0xFC,0x6B,0x20,0x00,0x00,0x60,0xAE,0x00,0x1E,0x00,0x1F,0x00,0x00,0x77,0x1E,0x00,0x00,0x00,0x00,0x1E,0x1E,0xF7,0xFF,0xFF,0xFF);
		udelay(100);
		lcm_dcs_write_seq_static(ctx,0xC2,0x48,0x3C,0x02,0x11,0xFB,0x54,0x02,0xA4,0x3F,0xFC,0xE8,0x84,0x3F,0xFE,0x1D,0x28,0x00,0x00,0x55,0x7C,0xC4,0x53,0x00,0x00,0x1F,0x00,0x1E,0x72,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xFF);
		udelay(100);
		lcm_dcs_write_seq_static(ctx,0xC3,0x4C,0x00,0x31,0x00,0x68,0x20,0x37,0x51,0x3F,0xA2,0x7B,0xE4,0x00,0x00,0xD3,0xDC,0x00,0x00,0x00,0x00,0x00,0x53,0x80,0x00,0xB1,0x90,0x79,0x23,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xFF);
		udelay(100);
		lcm_dcs_write_seq_static(ctx,0xC4,0x4A,0x3C,0x33,0xA9,0x95,0x16,0x37,0x51,0x00,0x33,0x69,0x76,0x00,0x33,0x63,0xF2,0x7F,0xDC,0x28,0xA2,0xE1,0x57,0x83,0xBE,0xAC,0x94,0x37,0x23,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xFF);
		udelay(100);
		lcm_dcs_write_seq_static(ctx,0xC5,0x46,0x00,0x37,0x51,0x70,0x7E,0x39,0x31,0x00,0x01,0xCD,0x40,0x3F,0x92,0x17,0xC0,0x00,0x00,0x00,0x00,0x00,0x1F,0x03,0xBC,0x00,0x04,0x37,0x7B,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xFF);
		udelay(100);
		lcm_dcs_write_seq_static(ctx,0xC6,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xFF);
		udelay(100);
		lcm_dcs_write_seq_static(ctx,0xC7,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xFF);
		udelay(100);
		lcm_dcs_write_seq_static(ctx,0xC8,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xFF);
		udelay(100);
		lcm_dcs_write_seq_static(ctx,0xC9,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xFF);
		udelay(100);
		lcm_dcs_write_seq_static(ctx,0xCA,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xFF);
		udelay(100);
		lcm_dcs_write_seq_static(ctx,0xCB,0x40,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xFF);
		udelay(100);
		lcm_dcs_write_seq_static(ctx,0xCC,0x40,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xFF);
		udelay(100);
		lcm_dcs_write_seq_static(ctx,0xCD,0x61,0x3C,0x05,0x10,0xF7,0x48,0x03,0xC1,0x3F,0xFE,0x0B,0xB0,0x3F,0xFE,0x37,0x48,0x00,0x00,0x2F,0xE6,0x80,0x0C,0x00,0x69,0x20,0x00,0x96,0x48,0x00,0x69,0x49,0x00,0x96,0x51,0xF7,0xFF,0xFF,0xFF);
		udelay(100);
		lcm_dcs_write_seq_static(ctx,0xCE,0x4D,0x00,0x05,0xA4,0x09,0x34,0x03,0xC1,0x3F,0xFC,0x52,0xC0,0x00,0x00,0xA4,0xB8,0x00,0x00,0x00,0x00,0x00,0x1D,0x00,0x69,0x52,0x00,0x96,0x7C,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xF7,0xFF,0xFF,0xFF);
		udelay(100);
		lcm_dcs_write_seq_static(ctx,0xCF,0x6B,0x3C,0x05,0xA4,0xF6,0xCC,0x03,0xC1,0x00,0x01,0xB1,0x98,0x00,0x02,0x42,0xDC,0x7F,0xFF,0x87,0x2A,0x04,0x1D,0x01,0x1E,0x52,0x01,0x4B,0x7C,0x00,0x97,0x20,0x01,0x1D,0x7C,0xF7,0xFF,0xFF,0xFF);
		udelay(100);
		lcm_dcs_write_seq_static(ctx,0xD0,0x67,0x00,0x05,0x10,0x08,0xB8,0x03,0xC1,0x00,0x00,0x2C,0xB0,0x3F,0xFC,0xAE,0xF0,0x00,0x00,0x00,0x00,0x00,0x0C,0x01,0x1E,0x20,0x01,0x4B,0x48,0x01,0x1E,0x49,0x01,0x4B,0x51,0xF7,0xFF,0xFF,0xFF);
		udelay(100);
		lcm_dcs_write_seq_static(ctx,0xFF,0x55,0xA5,0x81);
		udelay(100);
		lcm_dcs_write_seq_static(ctx,0x6F,0x0D);
		lcm_dcs_write_seq_static(ctx,0xF3,0xA7);
		lcm_dcs_write_seq_static(ctx,0x6F,0x2D);
		lcm_dcs_write_seq_static(ctx,0xFA,0x80);
		lcm_dcs_write_seq_static(ctx,0x6F,0x3E);
		lcm_dcs_write_seq_static(ctx,0xFA,0x01);
		lcm_dcs_write_seq_static(ctx,0x3B,0x00,0x10,0x00,0x10);
		udelay(100);
		lcm_dcs_write_seq_static(ctx,0x03,0x01);
		lcm_dcs_write_seq_static(ctx,0x90,0x01);
		lcm_dcs_write_seq_static(ctx,0x91,0x89, 0x28, 0x00, 0x14, 0xC2, 0x00, 0x03, 0x1C, 0x02, 0x8C, 0x00, 0x0F, 0x05, 0x0E, 0x02, 0x8B, 0x10, 0xF0);
		udelay(100);
		lcm_dcs_write_seq_static(ctx,0x2F,0x01);
		lcm_dcs_write_seq_static(ctx,0x26,0x01);
		// lcm_dcs_write_seq_static(ctx,0x51,0x07,0xFF,0x0F,0xFF);
		// lcm_dcs_write_seq_static(ctx,0x51,0x07,0xFF);
		udelay(100);
		lcm_dcs_write_seq_static(ctx,0x44,0x08,0xFC);
		udelay(100);
		lcm_dcs_write_seq_static(ctx,0x53,0x20);
		lcm_dcs_write_seq_static(ctx,0x53,0x20);
		lcm_dcs_write_seq_static(ctx,0x35,0x00);
		lcm_dcs_write_seq_static(ctx,0x2A,0x00, 0x00, 0x04, 0x37);
		udelay(100);
		lcm_dcs_write_seq_static(ctx,0x2A,0x00, 0x00, 0x04, 0x37);
		udelay(100);
		lcm_dcs_write_seq_static(ctx,0x2B,0x00, 0x00, 0x09, 0x23);
		udelay(100);
	#if 0
		lcm_dcs_write_seq_static(ctx,0xF0,0x55, 0xAA, 0x52, 0x08, 0x00);   //ELVSS
		udelay(100);
		lcm_dcs_write_seq_static(ctx,0xB5,0x80,0x80);
		lcm_dcs_write_seq_static(ctx,0x6F,0x07);
		lcm_dcs_write_seq_static(ctx,0xB5,0x23);
	#endif
	#if 1       //VGH
	lcm_dcs_write_seq_static(ctx,0xf0,0x55,0xaa,0x52,0x08,0x01);
	udelay(100);
	lcm_dcs_write_seq_static(ctx,0xB7,0x21,0x21);
	#endif
	}
}

static int mode_switch(struct drm_panel *panel, unsigned int cur_mode,
		unsigned int dst_mode, enum MTK_PANEL_MODE_SWITCH_STAGE stage)
{
	int ret = 0;
	struct drm_display_mode *m = get_mode_by_id(panel, dst_mode);
	printk("[wolf] %s,cur_mode = %d,dst_mode = %d\n",__func__,cur_mode,dst_mode);
	if (cur_mode == dst_mode)
		return ret;

	if (m->vrefresh == MODE_0_FPS) { /*switch to 60 */
		mode_switch_to_60(panel, stage);
	} else if (m->vrefresh == MODE_1_FPS) { /*switch to 90 */
		mode_switch_to_90(panel, stage);
	} else
		ret = 1;

	return ret;
}

static struct mtk_panel_funcs ext_funcs = {
	.reset = panel_ext_reset,
	.set_backlight_cmdq = lcm_setbacklight_cmdq,
	.ata_check = panel_ata_check,
	.hbm_set_cmdq = panel_hbm_set_cmdq,
	.hbm_get_state = panel_hbm_get_state,
	.get_virtual_heigh = lcm_get_virtual_heigh,
	.get_virtual_width = lcm_get_virtual_width,
	.ext_param_set = mtk_panel_ext_param_set,
	.mode_switch = mode_switch,
};
#endif

struct panel_desc {
	const struct drm_display_mode *modes;
	unsigned int num_modes;

	unsigned int bpc;

	struct {
		unsigned int width;
		unsigned int height;
	} size;

	struct {
		unsigned int prepare;
		unsigned int enable;
		unsigned int disable;
		unsigned int unprepare;
	} delay;
};
/*
static void change_drm_disp_mode_params(struct drm_display_mode *mode)
{
	if (fake_heigh > 0 && fake_heigh < VAC) {
		mode->vdisplay = fake_heigh;
		mode->vsync_start = fake_heigh + VFP;
		mode->vsync_end = fake_heigh + VFP + VSA;
		mode->vtotal = fake_heigh + VFP + VSA + VBP;
	}
	if (fake_width > 0 && fake_width < HAC) {
		mode->hdisplay = fake_width;
		mode->hsync_start = fake_width + HFP;
		mode->hsync_end = fake_width + HFP + HSA;
		mode->htotal = fake_width + HFP + HSA + HBP;
	}
}
*/
static int lcm_get_modes(struct drm_panel *panel)
{
	struct drm_display_mode *mode;
	struct drm_display_mode *mode_1;
 
	//if (need_fake_resolution)
	//	change_drm_disp_mode_params(&default_mode);
	mode = drm_mode_duplicate(panel->drm, &default_mode);
	if (!mode) {
		dev_err(panel->drm->dev, "failed to add mode %ux%ux@%u\n",
			default_mode.hdisplay, default_mode.vdisplay,
			default_mode.vrefresh);
		return -ENOMEM;																	
	}
	drm_mode_set_name(mode);
	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(panel->connector, mode);
	printk("[wolf] %s,111\n",__func__);

	mode_1 = drm_mode_duplicate(panel->drm, &switch_mode_90);
	if (!mode_1) {
		dev_err(panel->drm->dev, "failed to add mode_1 %ux%ux@%u\n",
			switch_mode_90.hdisplay, switch_mode_90.vdisplay,
			switch_mode_90.vrefresh);
		return -ENOMEM;																	
	}
	drm_mode_set_name(mode_1);
	mode_1->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(panel->connector, mode_1);	
	printk("[wolf] %s,222\n",__func__);
	panel->connector->display_info.width_mm = 69;
	panel->connector->display_info.height_mm = 151;

	return 1;
}

static const struct drm_panel_funcs lcm_drm_funcs = {
	.disable = lcm_disable,
	.unprepare = lcm_unprepare,
	.prepare = lcm_prepare,
	.enable = lcm_enable,
	.get_modes = lcm_get_modes,
};

static void check_is_need_fake_resolution(struct device *dev)
{
	unsigned int ret = 0;

	ret = of_property_read_u32(dev->of_node, "fake_heigh", &fake_heigh);
	if (ret)
		need_fake_resolution = false;
	ret = of_property_read_u32(dev->of_node, "fake_width", &fake_width);
	if (ret)
		need_fake_resolution = false;
	if (fake_heigh > 0 && fake_heigh < VAC)
		need_fake_resolution = true;
	if (fake_width > 0 && fake_width < HAC)
		need_fake_resolution = true;
	
	pr_err("%s------need_fake_resolution = %d------%d\n", __func__,need_fake_resolution,__LINE__);
}

static int lcm_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct lcm *ctx;
	struct device_node *backlight;
	int ret;
	struct device_node *dsi_node, *remote_node = NULL, *endpoint = NULL;
	
	pr_err("gezi ---------%d-----\n",__LINE__);

	dsi_node = of_get_parent(dev->of_node);
	if (dsi_node) {
		endpoint = of_graph_get_next_endpoint(dsi_node, NULL);
		if (endpoint) {
			remote_node = of_graph_get_remote_port_parent(endpoint);
			if (!remote_node) {
				pr_err("No panel connected,skip probe lcm\n");
				return -ENODEV;
			}
			pr_err("device node name:%s\n", remote_node->name);
		}
	}
	if (remote_node != dev->of_node) {
		pr_err("gezi ---- %s+ skip probe due to not current lcm\n", __func__);
		return -ENODEV;
	}

	ctx = devm_kzalloc(dev, sizeof(struct lcm), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;
 
	mipi_dsi_set_drvdata(dsi, ctx);

	ctx->dev = dev;
	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	// dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_SYNC_PULSE
	// 		 | MIPI_DSI_MODE_LPM | MIPI_DSI_MODE_EOT_PACKET
	// 		 | MIPI_DSI_CLOCK_NON_CONTINUOUS;
	dsi->mode_flags = MIPI_DSI_MODE_LPM | MIPI_DSI_MODE_EOT_PACKET;
	// dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST;
	backlight = of_parse_phandle(dev->of_node, "backlight", 0);
	if (backlight) {
		ctx->backlight = of_find_backlight_by_node(backlight);
		of_node_put(backlight);

		if (!ctx->backlight)
			return -EPROBE_DEFER;
	}
	
	pr_err("gezi ---------%d-----\n",__LINE__);

	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->reset_gpio)) {
		dev_err(dev, "%s: cannot get reset-gpios %ld\n",
			__func__, PTR_ERR(ctx->reset_gpio));
		return PTR_ERR(ctx->reset_gpio);
	}
	devm_gpiod_put(dev, ctx->reset_gpio);

	ctx->bias_pos = devm_gpiod_get_index(dev, "bias", 0, GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->bias_pos)) {
		dev_err(dev, "%s: cannot get bias-pos 0 %ld\n",
			__func__, PTR_ERR(ctx->bias_pos));
		return PTR_ERR(ctx->bias_pos);
	}
	devm_gpiod_put(dev, ctx->bias_pos);

	ctx->bias_neg = devm_gpiod_get_index(dev, "bias", 1, GPIOD_OUT_HIGH);
	if (IS_ERR(ctx->bias_neg)) {
		dev_err(dev, "%s: cannot get bias-neg 1 %ld\n",
			__func__, PTR_ERR(ctx->bias_neg));
		return PTR_ERR(ctx->bias_neg);
	}
	
	pr_err("gezi ---------%d-----\n",__LINE__);
	
	devm_gpiod_put(dev, ctx->bias_neg);

	ctx->prepared = true;
	ctx->enabled = true;

	drm_panel_init(&ctx->panel);
	ctx->panel.dev = dev;
	ctx->panel.funcs = &lcm_drm_funcs;

	ret = drm_panel_add(&ctx->panel);
	if (ret < 0)
		return ret;
	
	pr_err("gezi ---------%d-----\n",__LINE__);

	ret = mipi_dsi_attach(dsi);
	if (ret < 0)
		drm_panel_remove(&ctx->panel);

#if defined(CONFIG_MTK_PANEL_EXT)
	mtk_panel_tch_handle_reg(&ctx->panel);
	ret = mtk_panel_ext_create(dev, &ext_params, &ext_funcs, &ctx->panel);
	if (ret < 0)
		return ret;
#endif
	check_is_need_fake_resolution(dev);
	pr_err("%s------------%d\n", __func__,__LINE__);

	//add by wangfei 
	// lcm_panel_init(ctx);
	g_ctx = ctx;
	ctx->hbm_en = false;
	g_ctx->hbm_stat = false;
	
#if defined(CONFIG_PRIZE_HARDWARE_INFO)  
    strcpy(current_lcm_info.chip,"nt37700c.vdo");
    strcpy(current_lcm_info.vendor,"visionox");
    sprintf(current_lcm_info.id,"0x%02x",0x81);
    strcpy(current_lcm_info.more,"LCM");      
#endif
	return ret;
}

static int lcm_remove(struct mipi_dsi_device *dsi)
{
	struct lcm *ctx = mipi_dsi_get_drvdata(dsi);

	mipi_dsi_detach(dsi);
	drm_panel_remove(&ctx->panel);

	return 0;
}

static const struct of_device_id lcm_of_match[] = { 
	{ .compatible = "novatek,nt37700c,vdo", },
	{ }
};

MODULE_DEVICE_TABLE(of, lcm_of_match);

static struct mipi_dsi_driver lcm_driver = {
	.probe = lcm_probe,
	.remove = lcm_remove,
	.driver = {
		.name = "panel-nt37700c-vdo",
		.owner = THIS_MODULE,
		.of_match_table = lcm_of_match,
	},
};

module_mipi_dsi_driver(lcm_driver);

MODULE_AUTHOR("Yi-Lun Wang <Yi-Lun.Wang@mediatek.com>");
MODULE_DESCRIPTION("nt35695b VDO LCD Panel Driver");
MODULE_LICENSE("GPL v2");

