/*
 * C-SKY SoCs I2S Controller driver
 *
 * Copyright (C) 2017 C-SKY MicroSystems Co.,Ltd.
 *
 * Author: Lei Ling <lei_ling@c-sky.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/clk.h>
#include <linux/dmaengine.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/regmap.h>
#include <linux/reset.h>
#include <sound/dmaengine_pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-dai.h>
#include "csky-i2s.h"

#define BUFFER_BYTES_MAX	(512 * 1024)
#define PERIOD_BYTES_MIN	32
#define PERIOD_BYTES_MAX	(8 * 1024)
#define PERIODS_MIN		4
#define PERIODS_MAX		(BUFFER_BYTES_MAX / PERIOD_BYTES_MIN)

#define DEFAULT_FIFO_DEPTH		32 /* in words */
#define DEFAULT_INTR_TX_THRESHOLD	16
#define DEFAULT_INTR_RX_THRESHOLD	16
#define DEFAULT_DMA_TX_THRESHOLD	16
#define DEFAULT_DMA_RX_THRESHOLD	16

static const struct of_device_id csky_i2s_match[];

/*
 *               -------
 *    src_clk ---| div |--- mclk
 *               -------
 *
 * For csky,i2s-v1:
 *
 *              (left_j and i2s)
 *            |----- 1/8 --------|            -------
 *    mclk ---|                  |--- sclk ---| div |--- fs
 *            |----- 1/4 --------|            -------
 *              (right_j)
 *
 * For csky,i2s-v1.1:
 *            -------            -------
 *    mclk ---| div |--- sclk ---| div |--- fs
 *            -------            -------
 */
static const struct csky_i2s_params params_csky_i2s_v1 = {
	.has_mclk_sclk_div	= false,
};
static const struct csky_i2s_params params_csky_i2s_v1_1 = {
	.has_mclk_sclk_div	= true,
};

static int csky_i2s_calc_mclk_div(struct csky_i2s *i2s,
				  unsigned int rate,
				  unsigned int word_size)
{
	unsigned int mclk;
	int div;
	unsigned int mclk_fs_div;
	unsigned int tmp;

	mclk = i2s->mclk;
	if (mclk % rate) {
		dev_err(i2s->dev, "error! mclk is not a multiple of fs\n");
		return -EINVAL;
	}

	mclk_fs_div = mclk / rate;

	if (i2s->params.has_mclk_sclk_div) { /* For csky,i2s-v1.1 */
		if ((mclk_fs_div != 256) && (mclk_fs_div != 384) &&
		    (mclk_fs_div != 512) && (mclk_fs_div != 768)) {
			dev_err(i2s->dev, "error! invalid mclk_fs_div(%d)\n",
				mclk_fs_div);
			return -EINVAL;
		}
	} else { /* For csky,i2s-v1 */
		if ((i2s->audio_fmt == SND_SOC_DAIFMT_I2S) ||
		    (i2s->audio_fmt == SND_SOC_DAIFMT_LEFT_J)) {
			if (mclk_fs_div != 8 * i2s->sclk_fs_divider) {
				dev_err(i2s->dev, "error! mclk != 8*sclk\n");
				return -EINVAL;
			}
		} else {
			if (mclk_fs_div != 4 * i2s->sclk_fs_divider) {
				dev_err(i2s->dev, "error! mclk != 4*sclk\n");
				return -EINVAL;
			}
		}
	}

	i2s->mclk_fs_divider = mclk_fs_div;

	/* div = src_clk/(2*mclk) - 1; */
	div = i2s->src_clk / 2 / mclk - 1;
	if (div >= 0) {
		tmp = i2s->src_clk / (2 * (div + 1));
		if (tmp != mclk) {
			dev_err(i2s->dev,
				"error! mclk mismatch! Expected %d, got %d\n",
				mclk, tmp);
			return -EINVAL;
		}
	}

	return div;
}

static int csky_i2s_calc_spdifclk_div(struct csky_i2s *i2s,
				      unsigned int rate,
				      unsigned int word_size)
{
	/* DIV1_LEVEL[0X94] usually is configured as 17 or 11.  Why ? */
	return 17;
}

static int csky_i2s_calc_fs_div(struct csky_i2s *i2s, unsigned int word_size)
{
	unsigned int multi; /* sclk = multi * fs */
	int div;

	/* To support all the word sizes(16,24,32), fix sclk to 64fs */
	multi = 64;

	i2s->sclk_fs_divider = multi;

	/* div = sclk/(2*fs) - 1 = multi/2 - 1; */
	div = multi / 2 - 1;
	return div;
}

static int csky_i2s_calc_sclk_div(struct csky_i2s *i2s)
{
	unsigned int multi; /* mclk = multi * sclk */
	int div;

	multi = i2s->mclk_fs_divider / i2s->sclk_fs_divider;

	/* div = mclk/(2*sclk) - 1 = multi/2 - 1; */
	div = multi / 2 - 1;
	return div;
}

static int csky_i2s_calc_refclk_div(struct csky_i2s *i2s, unsigned int rate)
{
	unsigned int ref_clk;
	int div;

	switch (rate) {
	/* clk_domain_1/2/3: ref_clk = 3072khz */
	case 8000:
	case 16000:
	case 32000:
	case 48000:
	case 96000:
		ref_clk = 3072000;
		break;
	/* clk_domain_4: ref_clk = 2116.8khz */
	case 11025:
	case 22050:
	case 44100:
	case 88200:
		ref_clk = 2116800;
		break;
	default:
		return -EINVAL;
	}

	/* div = src_clk / (ref_clk*2) - 1; */
	div = i2s->src_clk / 2 / ref_clk - 1;
	return div;
}

static int csky_i2s_set_clk_rate(struct csky_i2s *i2s,
				 unsigned int rate,
				 unsigned int word_size)
{
	int mclk_div, spdifclk_div, fs_div, refclk_div, sclk_div;
	int ret;

	if (!IS_ERR_OR_NULL(i2s->i2s_clk)) {
		switch (rate) {
		case 8000:
		case 16000:
		case 32000:
		case 48000:
		case 96000:
			ret = clk_set_rate(i2s->i2s_clk, i2s->clk_fs_48k);
			if (ret)
				return ret;
			i2s->src_clk = clk_get_rate(i2s->i2s_clk);
			break;
		case 11025:
		case 22050:
		case 44100:
		case 88200:
			ret = clk_set_rate(i2s->i2s_clk, i2s->clk_fs_44k);
			if (ret)
				return ret;
			i2s->src_clk = clk_get_rate(i2s->i2s_clk);
			break;
		default:
			return -EINVAL;
		}
	}

	i2s->sample_rate = rate;

	fs_div = csky_i2s_calc_fs_div(i2s, word_size);
	if (fs_div < 0)
		return -EINVAL;

	mclk_div = csky_i2s_calc_mclk_div(i2s, rate, word_size);
	if (mclk_div < 0)
		return -EINVAL;

	if (i2s->params.has_mclk_sclk_div) {
		sclk_div = csky_i2s_calc_sclk_div(i2s);
		if (sclk_div < 0)
			return -EINVAL;
	}

	spdifclk_div = csky_i2s_calc_spdifclk_div(i2s, rate, word_size);
	if (spdifclk_div < 0)
		return -EINVAL;

	refclk_div = csky_i2s_calc_refclk_div(i2s, rate);
	if (refclk_div < 0)
		return -EINVAL;

	csky_i2s_writel(i2s, IIS_DIV0_LEVEL, mclk_div);
	csky_i2s_writel(i2s, IIS_DIV1_LEVEL, spdifclk_div);
	csky_i2s_writel(i2s, IIS_DIV2_LEVEL, fs_div);
	csky_i2s_writel(i2s, IIS_DIV3_LEVEL, refclk_div);
	if (i2s->params.has_mclk_sclk_div)
		csky_i2s_writel(i2s, IIS_DIV4_LEVEL, sclk_div);

	return 0;
}

static int csky_i2s_hw_params(struct snd_pcm_substream *substream,
			      struct snd_pcm_hw_params *params,
			      struct snd_soc_dai *dai)
{
	struct csky_i2s *i2s = snd_soc_dai_get_drvdata(dai);
	u32 width;
	u32 val;

	if (params_channels(params) > 2)
		return -EINVAL;

	val = csky_i2s_readl(i2s, IIS_FSSTA);
	val &= ~(FSSTA_RES_MASK << FSSTA_RES_SHIFT);

	switch (params_physical_width(params)) {
	case 16:
		if (params_channels(params) == 2)
			width = DMA_SLAVE_BUSWIDTH_4_BYTES;
		else
			width = DMA_SLAVE_BUSWIDTH_2_BYTES;

		val |= FSSTA_RES16_FIFO16;
		break;
	case 24:
		width = DMA_SLAVE_BUSWIDTH_3_BYTES;
		val |= FSSTA_RES24_FIFO24;
		break;
	case 32:
		width = DMA_SLAVE_BUSWIDTH_4_BYTES;
		val |= FSSTA_RES24_FIFO24;
		break;
	default:
		return -EINVAL;
	}
	i2s->playback_dma_data.addr_width = width;

	csky_i2s_writel(i2s, IIS_FSSTA, val);
	return csky_i2s_set_clk_rate(i2s, params_rate(params),
				     params_width(params));
}

static int csky_i2s_set_dai_sysclk(struct snd_soc_dai *dai,
				   int clk_id, unsigned int freq, int dir)
{
	struct csky_i2s *i2s = snd_soc_dai_get_drvdata(dai);

	i2s->mclk = freq;
	return 0;
}

static int csky_i2s_set_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	struct csky_i2s *i2s = snd_soc_dai_get_drvdata(dai);
	u32 val;

	val = csky_i2s_readl(i2s, IIS_IISCNF_OUT);
	val &= ~(OUT_AUDFMT_MASK << OUT_AUDFMT_SHIFT);
	val &= ~(OUT_WS_POLARITY_MASK << OUT_WS_POLARITY_SHIFT);
	val &= ~(OUT_M_S_MASK << OUT_M_S_SHIFT);

	/* DAI Mode */
	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_I2S:
		val |= IISCNF_OUT_AUDFMT_I2S;
		break;
	case SND_SOC_DAIFMT_LEFT_J:
		val |= IISCNF_OUT_AUDFMT_LEFT_J;
		break;
	case SND_SOC_DAIFMT_RIGHT_J:
		val |= IISCNF_OUT_AUDFMT_RIGHT_J;
		break;
	default:
		return -EINVAL;
	}

	/* DAI clock polarity */
	switch (fmt & SND_SOC_DAIFMT_INV_MASK) {
	case SND_SOC_DAIFMT_NB_NF:
		val |= IISCNF_OUT_WS_POLARITY_NORMAL;
		break;
	case SND_SOC_DAIFMT_NB_IF:
		/* Invert frame clock */
		val |= IISCNF_OUT_WS_POLARITY_INVERTED;
		break;
	case SND_SOC_DAIFMT_IB_IF:
	case SND_SOC_DAIFMT_IB_NF:
	default:
		return -EINVAL;
	}

	/* DAI clock master masks */
	switch (fmt & SND_SOC_DAIFMT_MASTER_MASK) {
	case SND_SOC_DAIFMT_CBS_CFS:
		/* BCLK and LRCLK master */
		val |= IISCNF_OUT_MASTER;
		break;
	case SND_SOC_DAIFMT_CBM_CFM:
		/* BCLK and LRCLK slave */
		val |= IISCNF_OUT_SLAVE;
		break;
	default:
		return -EINVAL;
	}

	csky_i2s_writel(i2s, IIS_IISCNF_OUT, val);
	i2s->audio_fmt = fmt & SND_SOC_DAIFMT_FORMAT_MASK;
	return 0;
}

/* apply to hdmi */
#ifdef CONFIG_CSKY_HDMI
extern void csky_hdmi_audio_config(unsigned int sample_rate, unsigned int audio_fmt);
#endif

static void csky_i2s_start_playback(struct csky_i2s *i2s)
{
	if (i2s->use_pio)
		csky_i2s_writel(i2s, IIS_IMR, IIS_FIFOINT_TX_FIFO_EMPTY);
	else
		csky_i2s_writel(i2s, IIS_DMACR, DMACR_EN_TX_DMA);

	csky_i2s_writel(i2s, IIS_AUDIOEN, AUDIOEN_IIS_EN);

#ifdef CONFIG_CSKY_HDMI
	/* applay to hdmi audio */
	if (i2s->config_hdmi == 1)
		csky_hdmi_audio_config(i2s->sample_rate, i2s->audio_fmt);
#endif
}

static void csky_i2s_stop_playback(struct csky_i2s *i2s)
{
	csky_i2s_writel(i2s, IIS_IMR, 0); /* disable fifo interrupts */
	csky_i2s_writel(i2s, IIS_DMACR, 0); /* disable tx dma */
	csky_i2s_writel(i2s, IIS_AUDIOEN, 0);
}

static int csky_i2s_trigger(struct snd_pcm_substream *substream,
			    int cmd,
			    struct snd_soc_dai *dai)
{
	struct csky_i2s *i2s = snd_soc_dai_get_drvdata(dai);

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_PAUSE_RELEASE:
	case SNDRV_PCM_TRIGGER_RESUME:
		if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
			csky_i2s_start_playback(i2s);
		else
			return -EINVAL;
		break;

	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
			csky_i2s_stop_playback(i2s);
		else
			return -EINVAL;
		break;

	default:
		return -EINVAL;
	}

	return 0;
}

static int csky_i2s_startup(struct snd_pcm_substream *substream,
			    struct snd_soc_dai *dai)
{
	return 0;
}

static void csky_i2s_shutdown(struct snd_pcm_substream *substream,
			      struct snd_soc_dai *dai)
{
}

static const struct snd_soc_dai_ops csky_i2s_dai_ops = {
	.set_fmt	= csky_i2s_set_fmt,
	.hw_params	= csky_i2s_hw_params,
	.set_sysclk	= csky_i2s_set_dai_sysclk,
	.trigger	= csky_i2s_trigger,
	.startup	= csky_i2s_startup,
	.shutdown	= csky_i2s_shutdown,
};

static void csky_i2s_init(struct csky_i2s *i2s)
{
	csky_i2s_writel(i2s, IIS_AUDIOEN, 0); /* disable I2S */

	csky_i2s_writel(i2s, IIS_FICR, IIS_FIFOINT_ALL); /* clear FIFO intr */
	csky_i2s_writel(i2s, IIS_CMIR, IIS_MODEINT_ALL); /* clear Mode intr */

	csky_i2s_writel(i2s, IIS_FSSTA,
			FSSTA_RATE_SET_BY_USER |
			FSSTA_RES16_FIFO16);

	/* set the center count of FS when ref_clk = 3.072MHz */
	csky_i2s_writel(i2s, IIS_FADTLR,
			FADTLR_48FTR(0x40) |
			FADTLR_44FTR(0x46) |
			FADTLR_32FTR(0x60) |
			FADTLR_96FTR(0x20));

	csky_i2s_writel(i2s, IIS_IMR, 0); /* disable FIFO intr */

	csky_i2s_writel(i2s, IIS_RXFTLR, i2s->intr_rx_threshold);
	if (i2s->use_pio)
		csky_i2s_writel(i2s, IIS_TXFTLR, i2s->intr_tx_threshold);
	else
		csky_i2s_writel(i2s, IIS_TXFTLR, 0);

	csky_i2s_writel(i2s, IIS_DMARDLR, i2s->dma_rx_threshold);
	csky_i2s_writel(i2s, IIS_DMATDLR, i2s->dma_tx_threshold);

	csky_i2s_writel(i2s, IIS_MIMR, 0x0); /* disable Mode intr */

	csky_i2s_writel(i2s, IIS_SCCR, 0x0); /* no sample compress */

	csky_i2s_writel(i2s, IIS_FUNCMODE,
			FUNCMODE_MODE_WEN |
			FUNCMODE_MODE_TX); /* tx mode */
	csky_i2s_writel(i2s, IIS_IISCNF_OUT,
			IISCNF_OUT_AUDFMT_I2S |
			IISCNF_OUT_WS_POLARITY_NORMAL |
			IISCNF_OUT_MASTER); /* master, i2s mode */
	return;
}

static int csky_i2s_dai_probe(struct snd_soc_dai *dai)
{
	struct csky_i2s *i2s = snd_soc_dai_get_drvdata(dai);

	snd_soc_dai_init_dma_data(dai,
				  &i2s->playback_dma_data,
				  NULL /* capture not supported yet */);

	snd_soc_dai_set_drvdata(dai, i2s);

	csky_i2s_init(i2s);
	return 0;
}

static struct snd_soc_dai_driver csky_i2s_dai = {
	.probe = csky_i2s_dai_probe,
	.playback = {
		.stream_name = "Playback",
		.channels_min = 1,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_8000_48000 |
			 SNDRV_PCM_RATE_88200 | SNDRV_PCM_RATE_96000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_U16_LE |
			   SNDRV_PCM_FMTBIT_S24_LE | SNDRV_PCM_FMTBIT_U24_LE,
	},
	.ops = &csky_i2s_dai_ops,
	.symmetric_rates = 1,
};

static const struct snd_soc_component_driver csky_i2s_component = {
	.name	= "csky-dai",
};

static const struct snd_pcm_hardware csky_pcm_dma_hardware = {
	.info		= (SNDRV_PCM_INFO_MMAP
			| SNDRV_PCM_INFO_MMAP_VALID
			| SNDRV_PCM_INFO_INTERLEAVED
			| SNDRV_PCM_INFO_BLOCK_TRANSFER
			| SNDRV_PCM_INFO_RESUME
			| SNDRV_PCM_INFO_PAUSE),
	.channels_min		= 1,
	.channels_max		= 2,
	.buffer_bytes_max	= BUFFER_BYTES_MAX,
	.period_bytes_min	= PERIOD_BYTES_MIN,
	.period_bytes_max	= PERIOD_BYTES_MAX,
	.periods_min		= PERIODS_MIN,
	.periods_max		= PERIODS_MAX,
};

static irqreturn_t csky_i2s_irq_handler(int irq, void *dev_id)
{
	struct csky_i2s *i2s = dev_id;
	u32 val;

	val = csky_i2s_readl(i2s, IIS_ISR);
	/* clear interrupts */
	csky_i2s_writel(i2s, IIS_FICR, val);

	if (val & IIS_FIFOINT_TX_FIFO_EMPTY) {
		if (i2s->use_pio) {
			csky_pcm_pio_push_tx(i2s);
			csky_i2s_writel(i2s, IIS_FICR, val);
		}
	}

	return IRQ_HANDLED;
}

static int csky_i2s_probe(struct platform_device *pdev)
{
	const struct of_device_id *match;
	struct csky_i2s *i2s;
	struct resource *res;
	struct snd_dmaengine_pcm_config *pcm_conf;
	struct clk *tmp_clk;
	int ret;

	i2s = devm_kzalloc(&pdev->dev, sizeof(*i2s), GFP_KERNEL);
	if (!i2s)
		return -ENOMEM;
	platform_set_drvdata(pdev, i2s);

	match = of_match_device(csky_i2s_match, &pdev->dev);
	if (match && match->data)
		memcpy(&i2s->params, match->data, sizeof(i2s->params));

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	i2s->regs = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(i2s->regs))
		return PTR_ERR(i2s->regs);

	i2s->irq = platform_get_irq(pdev, 0);
	if (i2s->irq < 0) {
		dev_err(&pdev->dev, "Failed to retrieve irq number\n");
		return i2s->irq;
	}

	ret = devm_request_irq(&pdev->dev, i2s->irq,
			       csky_i2s_irq_handler, 0, pdev->name, i2s);
	if (ret < 0) {
		dev_err(&pdev->dev, "Failed to request irq\n");
		return ret;
	}

	if (of_property_read_u32(pdev->dev.of_node, "clock-frequency",
				 &i2s->src_clk) < 0) {
		/* get i2s clk */

		i2s->i2s_clk = devm_clk_get(&pdev->dev, "audio");
		if (IS_ERR(i2s->i2s_clk)) {
			dev_err(&pdev->dev, "Failed to get clk 'audio'\n");
			return PTR_ERR(i2s->i2s_clk);
		}

		ret = clk_prepare_enable(i2s->i2s_clk);
		if (ret) {
			dev_err(&pdev->dev, "Failed to enable clk 'audio'\n");
			return ret;
		}

		i2s->src_clk = clk_get_rate(i2s->i2s_clk);

		/* get i2s clk gate */

		i2s->i2s_clk_gate = devm_clk_get(&pdev->dev, "gate");
		if (IS_ERR(i2s->i2s_clk_gate)) {
			dev_err(&pdev->dev, "Failed to get clk 'gate'\n");
			ret = PTR_ERR(i2s->i2s_clk_gate);
			goto err_clk;
		}

		ret = clk_prepare_enable(i2s->i2s_clk_gate);
		if (ret) {
			dev_err(&pdev->dev, "Failed to enable clk 'gate'\n");
			goto err_clk;
		}

		/* get clk for 44.1k fs */
		tmp_clk = devm_clk_get(&pdev->dev, "clk-for-fs-44k");
		if (IS_ERR(tmp_clk)) {
			dev_err(&pdev->dev,
				"Failed to get clk 'clk-for-fs-44k'\n");
			ret = PTR_ERR(tmp_clk);
			goto err_clk;
		}
		i2s->clk_fs_44k = clk_get_rate(tmp_clk);

		/* get clk for 48k fs */
		tmp_clk = devm_clk_get(&pdev->dev, "clk-for-fs-48k");
		if (IS_ERR(tmp_clk)) {
			dev_err(&pdev->dev,
				"Failed to get clk 'clk-for-fs-48k'\n");
			ret = PTR_ERR(tmp_clk);
			goto err_clk;
		}
		i2s->clk_fs_48k = clk_get_rate(tmp_clk);
	}

	if (of_find_property(pdev->dev.of_node, "dmas", NULL)) {
		dev_info(&pdev->dev, "use dma\n");
		i2s->use_pio = false;
	} else {
		dev_info(&pdev->dev, "use pio\n");
		i2s->use_pio = true;
	}

	if (of_property_read_u32(pdev->dev.of_node, "fifo-depth",
				 &i2s->fifo_depth) < 0)
		i2s->fifo_depth = DEFAULT_FIFO_DEPTH;
	if (of_property_read_u32(pdev->dev.of_node, "intr-tx-threshold",
				 &i2s->intr_tx_threshold) < 0)
		i2s->intr_tx_threshold = DEFAULT_INTR_TX_THRESHOLD;
	if (of_property_read_u32(pdev->dev.of_node, "intr-rx-threshold",
				 &i2s->intr_rx_threshold) < 0)
		i2s->intr_rx_threshold = DEFAULT_INTR_RX_THRESHOLD;
	if (of_property_read_u32(pdev->dev.of_node, "dma-tx-threshold",
				 &i2s->dma_tx_threshold) < 0)
		i2s->dma_tx_threshold = DEFAULT_DMA_TX_THRESHOLD;
	if (of_property_read_u32(pdev->dev.of_node, "dma-rx-threshold",
				 &i2s->dma_rx_threshold) < 0)
		i2s->dma_rx_threshold = DEFAULT_DMA_RX_THRESHOLD;

	of_property_read_u32(pdev->dev.of_node, "sclk-fs-divider",
			     &i2s->sclk_fs_divider);

	i2s->playback_dma_data.maxburst =
			i2s->fifo_depth - i2s->dma_tx_threshold;

	i2s->dev = &pdev->dev;
	i2s->playback_dma_data.addr = res->start + IIS_DR;
	i2s->audio_fmt = SND_SOC_DAIFMT_I2S;

	ret = devm_snd_soc_register_component(&pdev->dev,
					      &csky_i2s_component,
					      &csky_i2s_dai, 1);
	if (ret) {
		dev_err(&pdev->dev, "Failed to register DAI\n");
		goto err_clk;
	}

	pcm_conf = devm_kzalloc(&pdev->dev, sizeof(*pcm_conf), GFP_KERNEL);
	if (!pcm_conf) {
		dev_err(&pdev->dev, "Failed to allocate memory for pcm_conf\n");
		ret = -ENOMEM;
		goto err_clk;
	}

	if (i2s->use_pio) {
		ret = csky_pcm_pio_register(pdev);
		if (ret) {
			dev_err(&pdev->dev,
				"Could not register PIO PCM: %d\n", ret);
			goto err_clk;
		}
	} else {
		pcm_conf->prepare_slave_config =
				csky_snd_dmaengine_pcm_prepare_slave_config;
		pcm_conf->pcm_hardware = &csky_pcm_dma_hardware;
		pcm_conf->prealloc_buffer_size = BUFFER_BYTES_MAX;

		ret = csky_snd_dmaengine_pcm_register(&pdev->dev, pcm_conf, 0);
		if (ret) {
			dev_err(&pdev->dev, "Failed to register PCM\n");
			goto err_clk;
		}
	}

	if (of_property_read_bool(pdev->dev.of_node, "config-hdmi"))
		i2s->config_hdmi = 1;
	else
		i2s->config_hdmi = 0;

	return 0;

err_clk:
	if (!IS_ERR(i2s->i2s_clk))
		clk_disable_unprepare(i2s->i2s_clk);
	if (!IS_ERR(i2s->i2s_clk_gate))
		clk_disable_unprepare(i2s->i2s_clk_gate);

	return ret;
}

static int csky_i2s_remove(struct platform_device *pdev)
{
	struct csky_i2s *i2s = dev_get_drvdata(&pdev->dev);

	if (!IS_ERR(i2s->i2s_clk))
		clk_disable_unprepare(i2s->i2s_clk);
	if (!IS_ERR(i2s->i2s_clk_gate))
		clk_disable_unprepare(i2s->i2s_clk_gate);

	csky_snd_dmaengine_pcm_unregister(&pdev->dev);
	return 0;
}

static const struct of_device_id csky_i2s_match[] = {
	{ .compatible = "csky,i2s-v1", .data = &params_csky_i2s_v1 },
	{ .compatible = "csky,i2s-v1.1", .data = &params_csky_i2s_v1_1 },
	{}
};
MODULE_DEVICE_TABLE(of, csky_i2s_match);

static struct platform_driver csky_i2s_driver = {
	.probe	= csky_i2s_probe,
	.remove	= csky_i2s_remove,
	.driver	= {
		.name		= "csky-i2s",
		.of_match_table	= csky_i2s_match,
	},
};
module_platform_driver(csky_i2s_driver);

MODULE_DESCRIPTION("C-SKY SoCs I2S Controller Driver");
MODULE_AUTHOR("Lei Ling <lei_ling@c-sky.com>");
MODULE_LICENSE("GPL v2");
