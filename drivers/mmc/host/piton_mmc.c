// SPDX-License-Identifier: GPL-2.0
/*
 * Piton LiteSDCard driver
 *
 * Copyright (C) 2019-2020 Antmicro <contact@antmicro.com>
 * Copyright (C) 2019-2020 Kamil Rakoczy <krakoczy@antmicro.com>
 * Copyright (C) 2019-2020 Maciej Dudek <mdudek@internships.antmicro.com>
 * Copyright (C) 2020 Paul Mackerras <paulus@ozlabs.org>
 * Copyright (C) 2020-2022 Gabriel Somlo <gsomlo@gmail.com>
 */

#include "linux/compiler.h"
#include "linux/dev_printk.h"
#include "linux/device.h"
#include "linux/io.h"
#include "linux/scatterlist.h"
#include <linux/bits.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/iopoll.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>

#include <linux/mmc/host.h>
#include <linux/mmc/mmc.h>
#include <linux/mmc/sd.h>


struct piton_mmc_host {
	struct mmc_host *mmc;
    struct mmc_data *data;

	void __iomem *sdmem;

    uint32_t blk_addr;

    size_t buffer_bytes_left;
    size_t bytes_left;
	size_t buf_size;
    bool rw;

    size_t bytes_remain;
    size_t sg_len;
    struct scatterlist* sg;
};

static int piton_mmc_get_cd(struct mmc_host *mmc)
{
	return 1;
}


static void piton_mmc_request(struct mmc_host *mmc, struct mmc_request *req)
{
	struct piton_mmc_host *host = mmc_priv(mmc);
	struct device *dev = mmc_dev(mmc);
	struct mmc_command *cmd = req->cmd;

	struct mmc_data *data = req->data;
	struct scatterlist *sg;
    int i;

    if (data == NULL) return;

    dev_dbg(dev, "piton_mmc_request: cmd->opcode=%d, cmd->arg=%d, data->blocks=%d, data->blksz=%d, data->flags=%d, rw_op: %lx\n",
            cmd->opcode, cmd->arg, data->blocks, data->blksz, data->flags, (data->flags & MMC_DATA_WRITE));

    host->data = data;
    host->buf_size = 0;
    host->bytes_remain = data->blocks * data->blksz;
    host->sg_len = data->sg_len;
    host->sg = data->sg;
    host->rw = (data->flags & MMC_DATA_WRITE);
    host->blk_addr = cmd->arg;

    for_each_sg(data->sg, sg, data->sg_len, i) {
        void* sg_buf = sg_virt(sg);
        void __iomem* sd_blk_addr = host->sdmem + host->blk_addr + host->buf_size;
        dev_dbg(dev, "piton_mmc_request: sg[%d]: %p, %d\n", i, sg, sg->length);
        if (sg_is_last(sg)) {
            WARN_ON(sg->length != host->bytes_remain);
        }
        if (host->rw) {
            memcpy_toio( sd_blk_addr , sg_buf, sg->length);
        } else {
            memcpy_fromio(sg_buf, sd_blk_addr, sg->length);
        }
        host->buf_size += sg->length;
    }

    if (host->bytes_remain != host->buf_size) {
        dev_dbg(dev, "piton_mmc_request: bytes_remain: %lx, buf_size: %lx\n", host->bytes_remain, host->buf_size);
    }

    host->data = NULL;
    host->bytes_remain = 0;
    host->buf_size = 0;
    host->rw = false;
    host->blk_addr = 0;

	mmc_request_done(mmc, req);
}

static const struct mmc_host_ops piton_mmc_ops = {
	.get_cd = piton_mmc_get_cd,
	.request = piton_mmc_request,
};

static void piton_mmc_free_host_wrapper(void *mmc)
{
	mmc_free_host(mmc);
}

static int piton_mmc_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct piton_mmc_host *host;
	struct mmc_host *mmc;
    struct resource *res;
	int ret;
    pr_info("piton_mmc_probe\n");

    res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
    res->end = res->start + 0x1000000;

    mmc = mmc_alloc_host(sizeof(struct piton_mmc_host), dev);
	if (!mmc)
		return -ENOMEM;

    host = mmc_priv(mmc);

    host->sdmem = devm_ioremap_resource(dev, res);
    if (IS_ERR(host->sdmem)) {
        dev_err(dev, "failed to remap sdmem\n");
        return PTR_ERR(host->sdmem);
    }

	ret = devm_add_action_or_reset(dev, piton_mmc_free_host_wrapper, mmc);
	if (ret)
		return dev_err_probe(dev, ret,
				     "Can't register mmc_free_host action\n");

	host = mmc_priv(mmc);
	host->mmc = mmc;


	mmc->ops = &piton_mmc_ops;
    //FIXME
	mmc->ocr_avail = MMC_VDD_32_33 | MMC_VDD_33_34;

    ret = mmc_regulator_get_supply(mmc);
	if (ret || mmc->ocr_avail == 0) {
		dev_warn(dev, "can't get voltage, defaulting to 3.3V\n");
		mmc->ocr_avail = MMC_VDD_32_33 | MMC_VDD_33_34;
	}

	/*
	 * Set default sd_clk frequency range based on empirical observations
	 * of LiteSDCard gateware behavior on typical SDCard media
	 */
	mmc->f_min = 12.5e6;
	mmc->f_max = 50e6;


	/* Force 4-bit bus_width (only width supported by hardware) */
	mmc->caps = 0;
	mmc->caps |= MMC_CAP_4_BIT_DATA;

	/* Set default capabilities */
	mmc->caps2 = MMC_CAP2_NO_WRITE_PROTECT |
		      MMC_CAP2_NO_SDIO |
		      MMC_CAP2_NO_MMC;

	mmc->max_blk_size = 2048;
	mmc->max_blk_count = 65535;
	mmc->max_req_size = mmc->max_blk_size * mmc->max_blk_count;
	mmc->max_seg_size = mmc->max_req_size;

	ret = mmc_of_parse(mmc);
	if (ret)
		return ret;

	ret = mmc_add_host(mmc);
	if (ret)
		return ret;

	dev_info(dev, "Piton MMC controller initialized.\n");
	return 0;
}

static int piton_mmc_remove(struct platform_device *pdev)
{
	struct piton_mmc_host *host = platform_get_drvdata(pdev);

	mmc_remove_host(host->mmc);
	return 0;
}

static const struct of_device_id piton_match[] = {
	{ .compatible = "openpiton,piton-mmc" },
	{ }
};
MODULE_DEVICE_TABLE(of, piton_match);

static struct platform_driver piton_mmc_driver = {
	.probe = piton_mmc_probe,
	.remove = piton_mmc_remove,
	.driver = {
		.name = "piton-mmc",
		.of_match_table = piton_match,
	},
};
module_platform_driver(piton_mmc_driver);

MODULE_DESCRIPTION("OpenPiton SDCard driver");
MODULE_AUTHOR("Tianrui Wei <tianruiwei@eecs.berkeley.edu>");
MODULE_LICENSE("GPL v2");
