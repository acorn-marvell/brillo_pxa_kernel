/*
 * DVC (dynamic voltage change) driver for Marvell 88PM88x
 * - Derived from dvc driver for 88pm800/88pm822/88pm860
 *
 * Copyright (C) 2014 Marvell International Ltd.
 * Joseph(Yossi) Hanin <yhanin@marvell.com>
 * Yi Zhang <yizhang@marvell.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/err.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <linux/mfd/88pm88x.h>
#include <linux/mfd/88pm886.h>
#include <linux/mfd/88pm880.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/of.h>

/* buck dvc sets register */
#define PM880_BUCK1B_VOUT		(0x40)
#define PM880_BUCK7_VOUT		(0xb8)

/* to enable dvc*/
#define PM88X_PWR_HOLD			(1 << 7)

#define BUCK_MIN_VOLT			(600000)
#define BUCK_MAX_VOLT			(1587500)
#define BUCK_STEP			(12500)

static struct pm88x_dvc *g_dvc;

struct pm88x_dvc_print {
	char name[15];
	char sets[16][10];
};

struct pm88x_dvc_extra {
	char *name;
	struct pm88x_dvc *dvc;
};

struct pm88x_dvc_desc {
	char *name;
	char pin_status[10];
	unsigned int mode_reg;
	unsigned int mode_mask;
	unsigned int val_reg;
	unsigned int val_mask;
};

#define PM88X_DVC_DESC(_name, _mode_reg, _mode_mask, _val_reg, _val_mask)	\
{										\
	.name		= _name,						\
	.mode_reg	= _mode_reg,						\
	.mode_mask	= _mode_mask,						\
	.val_reg	= _val_reg,						\
	.val_mask	= _val_mask,						\
}

static struct pm88x_dvc_desc pm886_dvc_desc_info[] = {
	PM88X_DVC_DESC("DVC1", 0x32, 0x0e, 0x32, 0x01),
	PM88X_DVC_DESC("DVC2", 0x32, 0xe0, 0x32, 0x10),
	PM88X_DVC_DESC("DVC3", 0x30, 0xe0, 0x30, 0x10),
};

static struct pm88x_dvc_desc pm880_dvc_desc_info[] = {
	PM88X_DVC_DESC("DVC1", 0x32, 0x0e, 0x32, 0x01),
	PM88X_DVC_DESC("DVC2", 0x32, 0xe0, 0x32, 0x10),
	PM88X_DVC_DESC("DVC3", 0x33, 0x0e, 0x33, 0x01),
	PM88X_DVC_DESC("DVC4", 0x33, 0xe0, 0x33, 0x10),
	PM88X_DVC_DESC("DVC5", 0x34, 0x0e, 0x34, 0x01),
	PM88X_DVC_DESC("DVC6", 0x34, 0xe0, 0x34, 0x10),
};

struct pm88x_dvc buck1b_dvc = {
	.desc.current_reg = PM880_BUCK1B_VOUT,
	.desc.mid_reg_val = 0x50,
	.desc.max_level = 16,
	.desc.uV_step1 = 12500,
	.desc.uV_step2 = 50000,
	.desc.min_uV = 600000,
	.desc.mid_uV = 1600000,
	.desc.max_uV = 1800000,
};

struct pm88x_dvc buck7_dvc = {
	.desc.current_reg = PM880_BUCK7_VOUT,
	.desc.mid_reg_val = 0x50,
	.desc.max_level = 4,
	.desc.uV_step1 = 12500,
	.desc.uV_step2 = 50000,
	.desc.min_uV = 600000,
	.desc.mid_uV = 1600000,
	.desc.max_uV = 3300000,
};

static inline int map_volt_to_reg(int uv)
{
	if (uv >= g_dvc->desc.mid_uV)
		return (uv - g_dvc->desc.mid_uV) / (g_dvc->desc.uV_step2) + g_dvc->desc.mid_reg_val;
	else
		return (uv - g_dvc->desc.min_uV) / (g_dvc->desc.uV_step1);
}

static inline int list_volt_from_reg(int reg_val)
{
	if (reg_val >= g_dvc->desc.mid_reg_val) {
		reg_val -= g_dvc->desc.mid_reg_val;
		return reg_val * (g_dvc->desc.uV_step2) + g_dvc->desc.mid_uV;
	} else
		return reg_val * (g_dvc->desc.uV_step1) + g_dvc->desc.min_uV;
}

static void pm886_level_to_reg(u8 level)
{
	if (level < 0 || level >= g_dvc->desc.max_level) {
		dev_err(g_dvc->dev, "DVC level is out of range!\n");
		return;
	}

	if (level < 4)
		g_dvc->desc.current_reg = PM886_BUCK1_VOUT + level;

	else
		g_dvc->desc.current_reg = PM886_BUCK1_4_VOUT + level - 4;
}

static void pm880_level_to_reg(u8 level)
{
	if (level < 0 || level >= g_dvc->desc.max_level) {
		dev_err(g_dvc->dev, "DVC level is out of range!\n");
		return;
	}

	g_dvc->desc.current_reg = PM880_BUCK1_VOUT + level;
}

static void pm880_buck1b_level_to_reg(u8 level)
{
	if (level < 0 || level >= buck1b_dvc.desc.max_level) {
		dev_err(g_dvc->dev, "DVC level is out of range!\n");
		return;
	}

	g_dvc->desc.current_reg = PM880_BUCK1B_VOUT + level;
}

static void pm880_buck7_level_to_reg(u8 level)
{
	if (level < 0 || level >= buck7_dvc.desc.max_level) {
		dev_err(g_dvc->dev, "DVC level is out of range!\n");
		return;
	}

	g_dvc->desc.current_reg = PM880_BUCK7_VOUT + level;
}

/*
 * Example for usage: set buck1 level1 as 1200mV
 * pm88x_dvc_set_volt(1, 1200 * 1000);
 * level begins with 0
 */
int pm88x_dvc_set_volt(u8 level, int uv)
{
	int ret = 0;
	struct regmap *regmap;

	if (!g_dvc || !g_dvc->chip || !g_dvc->chip->buck_regmap) {
		pr_err("%s: NULL pointer!\n", __func__);
		return -EINVAL;
	}
	regmap = g_dvc->chip->buck_regmap;

	if (uv < g_dvc->desc.min_uV || uv > g_dvc->desc.max_uV) {
		dev_err(g_dvc->dev, "the expected voltage is out of range!\n");
		return -EINVAL;
	}

	g_dvc->ops.level_to_reg(level);

	ret = regmap_update_bits(regmap, g_dvc->desc.current_reg, 0x7f,
				 map_volt_to_reg(uv));
	return ret;
};
EXPORT_SYMBOL(pm88x_dvc_set_volt);

/*
 * Example for usage: get buck1 voltage for special level
 * volt = pm88x_dvc_get_volt(level);
 * level begins with 0
 */
int pm88x_dvc_get_volt(u8 level)
{
	struct regmap *regmap;
	int ret = 0, regval = 0;
	int volt;

	if (!g_dvc || !g_dvc->chip || !g_dvc->chip->buck_regmap) {
		pr_err("%s: NULL pointer!\n", __func__);
		return -EINVAL;
	}

	g_dvc->ops.level_to_reg(level);

	regmap = g_dvc->chip->buck_regmap;
	ret = regmap_read(regmap, g_dvc->desc.current_reg, &regval);
	if (ret < 0) {
		dev_err(g_dvc->dev, "fail to read reg: 0x%x\n",
			g_dvc->desc.current_reg);
		return ret;
	}
	regval &= 0x7f;

	volt = list_volt_from_reg(regval);
	if (volt < g_dvc->desc.min_uV || volt > g_dvc->desc.max_uV) {
		dev_err(g_dvc->dev,
			"voltage out of range: %duV, level = %d\n", volt, level);
		return g_dvc->desc.min_uV;
	}

	return volt;
}
EXPORT_SYMBOL(pm88x_dvc_get_volt);

/* configure according to chip type */
static int pm88x_dvc_chip_init(struct pm88x_dvc *dvc)
{
	int ret = -EINVAL;

	if (!dvc || !dvc->chip)
		return -EINVAL;

	switch (dvc->chip->type) {
	case PM886:
		/* config gpio1 as DVC3 pin for 88pm886 */
		ret = regmap_update_bits(dvc->chip->base_regmap,
					 PM88X_GPIO_CTRL1,
					 PM88X_GPIO1_MODE_MSK,
					 PM88X_GPIO1_SET_DVC);
		if (ret < 0) {
			dev_err(dvc->dev, "Failed to set gpio1 as dvc pin!\n");
			return ret;
		}
		dvc->desc.current_reg = PM886_BUCK1_VOUT;
		dvc->desc.mid_reg_val = 0x50;
		dvc->desc.max_level = 8;
		dvc->desc.uV_step1 = 12500;
		dvc->desc.uV_step2 = 50000;
		dvc->desc.min_uV = 600000;
		dvc->desc.mid_uV = 1600000;
		dvc->desc.max_uV = 1800000;

		dvc->ops.level_to_reg = pm886_level_to_reg;
		break;
	case PM880:
		/* TODO: configure gpio to dvc function for 88pm880 */
		dvc->desc.current_reg = PM880_BUCK1_VOUT;
		dvc->desc.mid_reg_val = 0x50;
		dvc->desc.max_level = 16;
		dvc->desc.uV_step1 = 12500;
		dvc->desc.uV_step2 = 50000;
		dvc->desc.min_uV = 600000;
		dvc->desc.mid_uV = 1600000;
		dvc->desc.max_uV = 1800000;

		dvc->ops.level_to_reg = pm880_level_to_reg;
		break;
	default:
		return ret;
	}

	return 0;
}

static int pm88x_update_print(struct pm88x_chip *chip, struct pm88x_dvc_extra *extra,
			      struct pm88x_dvc_print *print_temp, int index, int sets_num)
{
	struct regmap *regmap;
	int reg, volt, ret = 0, val = 0;

	regmap = chip->buck_regmap;

	sprintf(print_temp->name, "%s", extra->name);

	if (!index)
		volt = pm88x_dvc_get_volt(sets_num);
	else {
		reg = extra->dvc->desc.current_reg + sets_num;
		ret = regmap_read(regmap, reg, &val);
		if (ret < 0) {
			pr_err("fail to read reg: 0x%x\n", extra->dvc->desc.current_reg);
			return ret;
		}
		val &= 0x7f;

		volt = list_volt_from_reg(val);
		if (volt < extra->dvc->desc.min_uV || volt > extra->dvc->desc.max_uV) {
			pr_err("voltage out of range: %duV, level = %d\n", volt, sets_num);
			return extra->dvc->desc.min_uV;
		}
	}

	if (volt < 0)
		return volt;
	else
		sprintf(print_temp->sets[sets_num], "%4d", volt);

	return 0;
}

static int pm88x_dvc_sts_print(struct pm88x_chip *chip, struct pm88x_dvc_desc *desc, int num)
{
	struct regmap *regmap;
	int i, j, ret = 0, val = 0;
	u8 vals[2][5] = { {0x30, 0x31, 0x32, 0x33, 0x34} };

	regmap = chip->base_regmap;

	ret = regmap_bulk_read(regmap, 0x30, vals[1], 5);
	if (ret < 0)
		return ret;

	for (i = 0; i < num; i++) {
		for (j = 0; j < 5; j++) {
			if (desc[i].mode_reg == vals[0][j]) {
				val = vals[1][j];

				if (((val & desc[i].mode_mask) >>
						(ffs(desc[i].mode_mask) - 1)) == 0x02) {
					sprintf(desc[i].pin_status, "%7d", (val & desc[i].val_mask)
							>> (ffs(desc[i].val_mask) - 1));
				} else
					sprintf(desc[i].pin_status, "Disable");
			}
		}
	}
	return ret;
}

int pm88x_display_dvc(struct pm88x_chip *chip, char *buf)
{
	struct pm88x_dvc_print *print_temp;
	struct pm88x_dvc_extra extra[3];
	struct pm88x_dvc_desc *desc;
	int i, j, extra_num, sets_num, desc_num, len = 0;
	ssize_t ret;
	switch (chip->type) {
	case PM886:
		desc = pm886_dvc_desc_info;
		desc_num = ARRAY_SIZE(pm886_dvc_desc_info);
		extra[0].name = "BUCK1";
		extra[0].dvc = g_dvc;
		extra_num = 1;
		sets_num = 8;
		break;
	case PM880:
		desc = pm880_dvc_desc_info;
		desc_num = ARRAY_SIZE(pm880_dvc_desc_info);
		extra[0].name = "BUCK1A";
		extra[0].dvc = g_dvc;
		extra[1].name = "BUCK1B";
		extra[1].dvc = &buck1b_dvc;
		extra[2].name = "BUCK7";
		extra[2].dvc = &buck7_dvc;
		extra_num = 3;
		sets_num = 16;
		break;
	default:
		pr_err("%s: Cannot find chip type.\n", __func__);
		return -ENODEV;
	}

	print_temp = kmalloc(sizeof(struct pm88x_dvc_print), GFP_KERNEL);
	if (!print_temp) {
		pr_err("%s: Cannot allocate print template.\n", __func__);
		return -ENOMEM;
	}

	len += sprintf(buf + len, "\nDynamic Voltage Setting\n");

	for (i = desc_num; i > 0; i--)
		len += sprintf(buf + len, "----------");
	len += sprintf(buf + len, "-\n|");

	for (i = desc_num; i > 0; i--)
		len += sprintf(buf + len, "   DVC%d  |", i);
	len += sprintf(buf + len, "\n");

	for (i = desc_num; i > 0; i--)
		len += sprintf(buf + len, "----------");
	len += sprintf(buf + len, "-\n|");

	ret = pm88x_dvc_sts_print(chip, desc, desc_num);
	if (ret < 0) {
		pr_err("Print of DVC%d status failed\n", i + 1);
		goto out_print;
	}
	for (i = desc_num - 1; i >= 0; i--)
		len += sprintf(buf + len, " %-7s |", desc[i].pin_status);

	len += sprintf(buf + len, "\n-");
	for (i = desc_num; i > 0; i--)
		len += sprintf(buf + len, "----------");

	len += sprintf(buf + len, "\n--------");
	for (i = 0; i < extra_num; i++)
		len += sprintf(buf + len, "----------");

	len += sprintf(buf + len, "\n| set# |");
	for (i = 0; i < extra_num; i++)
		len += sprintf(buf + len, " %-7s |", extra[i].name);
	len += sprintf(buf + len, "\n--------");
	for (i = 0; i < extra_num; i++)
		len += sprintf(buf + len, "----------");

	for (i = 0; i < sets_num; i++) {
		len += sprintf(buf + len, "\n| set%X |", i);
		for (j = 0; j < extra_num; j++) {
			if (i < extra[j].dvc->desc.max_level) {
				ret = pm88x_update_print(chip, &extra[j], print_temp, j, i);
				if (ret < 0) {
					pr_err("Print of DVC %s failed\n",
					       print_temp->name);
					goto out_print;
				}
				len += sprintf(buf + len, " %-7s |",
					       print_temp->sets[i]);
			} else {
				len += sprintf(buf + len, "    -    |");
			}
		}
	}

	len += sprintf(buf + len, "\n--------");
	for (i = 0; i < extra_num; i++)
		len += sprintf(buf + len, "----------");
	len += sprintf(buf + len, "\n");

	ret = len;
out_print:
	kfree(print_temp);
	return ret;
}

static int pm88x_dvc_update_volt(struct pm88x_chip *chip, struct pm88x_dvc_extra *extra,
				 struct pm88x_debug_info *info, int index)
{
	struct regmap *regmap = chip->buck_regmap;
	int i, start_lvl, end_lvl, ret = 0;

	if (info->lvl >= 0) {
		start_lvl = info->lvl;
		end_lvl = info->lvl + 1;
	} else {
		start_lvl = 0;
		end_lvl = extra->dvc->desc.max_level;
	}

	for (i = start_lvl; i < end_lvl; i++) {
		if (!index) {
			pm88x_dvc_set_volt(i, info->lvl_volt);
			pr_info("DVC: %s, level: %d voltage is set to %d mV.\n",
				info->name, i, info->lvl_volt / 1000);
		} else {
			if (info->lvl_volt < extra->dvc->desc.min_uV ||
				info->lvl_volt > extra->dvc->desc.max_uV) {
				dev_err(g_dvc->dev, "the expected voltage is out of range!\n");
				return -EINVAL;
			}

			extra->dvc->ops.level_to_reg(i);

			ret = regmap_update_bits(regmap, extra->dvc->desc.current_reg,
						 0x7f, map_volt_to_reg(info->lvl_volt));
			if (ret < 0)
				return ret;
			pr_info("DVC: %s, level: %d voltage is set to %d mV.\n",
				info->name, i, info->lvl_volt / 1000);
		}
	}

	return ret;
}

int pm88x_dvc_debug_write(struct pm88x_chip *chip, char *buf, struct pm88x_debug_info *info)
{
	struct pm88x_dvc_extra extra[3];
	int i, extra_num, ret = 0, name_flag = 0, setting_flag = 0;

	switch (chip->type) {
	case PM886:
		extra[0].name = "BUCK1";
		extra[0].dvc = g_dvc;
		extra_num = 1;
		break;
	case PM880:
		buck1b_dvc.ops.level_to_reg = pm880_buck1b_level_to_reg;
		buck7_dvc.ops.level_to_reg = pm880_buck7_level_to_reg;
		extra[0].name = "BUCK1A";
		extra[0].dvc = g_dvc;
		extra[1].name = "BUCK1B";
		extra[1].dvc = &buck1b_dvc;
		extra[2].name = "BUCK7";
		extra[2].dvc = &buck7_dvc;
		extra_num = 3;
		break;
	default:
		pr_err("%s: Cannot find chip type.\n", __func__);
		return -ENODEV;
	}

	for (i = 0; i < extra_num; i++) {
		if (!strcmp(info->name, extra[i].name)) {
			name_flag = 1;
			if (info->lvl_volt > 0) {
				setting_flag = 1;
				ret = pm88x_dvc_update_volt(chip, &extra[i], info, i);
				if (ret < 0)
					return ret;
			}
		}
	}

	if (!name_flag && info->name[0])
		pr_err("DVC: name does not exist.\n");
	else if (!setting_flag)
		ret = pm88x_display_dvc(chip, buf);

	return ret;
}

static int pm88x_dvc_probe(struct platform_device *pdev)
{
	struct pm88x_chip *chip = dev_get_drvdata(pdev->dev.parent);
	static struct pm88x_dvc *dvcdata;
	int ret;

	dvcdata = devm_kzalloc(&pdev->dev, sizeof(*g_dvc), GFP_KERNEL);
	if (!dvcdata) {
		dev_err(&pdev->dev, "Failed to allocate g_dvc");
		return -ENOMEM;
	}

	platform_set_drvdata(pdev, dvcdata);

	/* get global handler */
	g_dvc = dvcdata;
	g_dvc->chip = chip;
	g_dvc->dev = &pdev->dev;

	chip->dvc = g_dvc;

	ret = pm88x_dvc_chip_init(g_dvc);
	if (ret < 0) {
		dev_err(&pdev->dev, "Failed to do chip specific dvc setting!\n");
		return ret;
	}

	/* enable dvc feature */
	ret = regmap_update_bits(chip->base_regmap, PM88X_MISC_CONFIG1,
				 PM88X_PWR_HOLD, PM88X_PWR_HOLD);
	if (ret < 0) {
		dev_err(&pdev->dev, "Failed to enable pmic dvc feature!\n");
		return ret;
	}

	return 0;
}

static int pm88x_dvc_remove(struct platform_device *pdev)
{
	struct pm88x_dvc *dvcdata = platform_get_drvdata(pdev);
	devm_kfree(&pdev->dev, dvcdata);
	return 0;
}

static struct of_device_id pm88x_dvc_of_match[] = {
	{.compatible = "marvell,88pm88x-dvc"},
	{},
};

MODULE_DEVICE_TABLE(of, pm88x_dvc_of_match);

static struct platform_driver pm88x_dvc_driver = {
	.driver = {
		   .name = "88pm88x-dvc",
		   .owner = THIS_MODULE,
		   .of_match_table = of_match_ptr(pm88x_dvc_of_match),
		   },
	.probe = pm88x_dvc_probe,
	.remove = pm88x_dvc_remove,
};

static int pm88x_dvc_init(void)
{
	return platform_driver_register(&pm88x_dvc_driver);
}
subsys_initcall(pm88x_dvc_init);

static void pm88x_dvc_exit(void)
{
	platform_driver_unregister(&pm88x_dvc_driver);
}
module_exit(pm88x_dvc_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("DVC Driver for Marvell 88PM886 PMIC");
MODULE_ALIAS("platform:88pm88x-dvc");
