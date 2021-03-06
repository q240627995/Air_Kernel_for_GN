/*
 * sdp4430.c  --  SoC audio for TI OMAP4430 SDP
 *
 * Author: Misael Lopez Cruz <x0052729@ti.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 */

#include <linux/clk.h>
#include <linux/platform_device.h>
#include <linux/i2c.h>
#include <linux/regulator/consumer.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/jack.h>
#include <sound/soc-dsp.h>

#include <asm/mach-types.h>
#include <plat/hardware.h>
#include <plat/mux.h>
#include <plat/mcbsp.h>

#include <linux/gpio.h>
#include "omap-mcpdm.h"
#include "omap-abe.h"
#include "omap-abe-dsp.h"
#include "omap-pcm.h"
#include "omap-mcbsp.h"
#include "../codecs/twl6040.h"

#include "../../../arch/arm/mach-omap2/board-tuna.h"

#define TUNA_MAIN_MIC_GPIO  48
#define TUNA_SUB_MIC_GPIO   171

static int twl6040_power_mode;
static int mcbsp_cfg;
static struct snd_soc_codec *twl6040_codec;
struct regulator *twl6040_clk32kreg;

int omap4_tuna_get_type(void);

static int main_mic_bias_event(struct snd_soc_dapm_widget *w,
			struct snd_kcontrol *kcontrol, int event)
{
	gpio_set_value(TUNA_MAIN_MIC_GPIO, SND_SOC_DAPM_EVENT_ON(event));
	return 0;
}

static int sub_mic_bias_event(struct snd_soc_dapm_widget *w,
			struct snd_kcontrol *kcontrol, int event)
{
	gpio_set_value(TUNA_SUB_MIC_GPIO, SND_SOC_DAPM_EVENT_ON(event));
	return 0;
}

static int sdp4430_modem_mcbsp_configure(struct snd_pcm_substream *substream,
				struct snd_pcm_hw_params *params, int flag)
{
	int ret = 0;
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_pcm_substream *modem_substream[2];
	struct snd_soc_pcm_runtime *modem_rtd;
	int channels;

	if (flag) {
		modem_substream[substream->stream] =
		snd_soc_get_dai_substream(rtd->card,
						OMAP_ABE_BE_MM_EXT1,
						substream->stream);
		if (unlikely(modem_substream[substream->stream] == NULL))
			return -ENODEV;

		modem_rtd =
			modem_substream[substream->stream]->private_data;

		if (!mcbsp_cfg) {
			if (omap4_tuna_get_type() == TUNA_TYPE_TORO) {
				/* Set cpu DAI configuration */
				ret = snd_soc_dai_set_fmt(modem_rtd->cpu_dai,
						SND_SOC_DAIFMT_I2S |
						SND_SOC_DAIFMT_NB_NF |
						SND_SOC_DAIFMT_CBS_CFS);
				if (unlikely(ret < 0)) {
					printk(KERN_ERR "can't set Modem cpu DAI format\n");
					goto exit;
				}

				/* McBSP2 fclk reparented to ABE_24M_FCLK */
				ret = snd_soc_dai_set_sysclk(modem_rtd->cpu_dai,
						OMAP_MCBSP_SYSCLK_CLKS_FCLK,
						32 * 96 * params_rate(params),
						SND_SOC_CLOCK_IN);
				if (unlikely(ret < 0)) {
					printk(KERN_ERR "can't set Modem cpu DAI sysclk\n");
					goto exit;
				}

				/* assuming McBSP2 is S16_LE stereo */
				ret = snd_soc_dai_set_clkdiv(modem_rtd->cpu_dai, 0, 96);
				if (unlikely(ret < 0)) {
					printk(KERN_ERR "can't set Modem cpu DAI clkdiv\n");
					goto exit;
				}
			} else {
				/* Set cpu DAI configuration */
				ret = snd_soc_dai_set_fmt(modem_rtd->cpu_dai,
						SND_SOC_DAIFMT_I2S |
						SND_SOC_DAIFMT_NB_NF |
						SND_SOC_DAIFMT_CBM_CFM);

				if (unlikely(ret < 0)) {
					printk(KERN_ERR "can't set Modem cpu DAI configuration\n");
					goto exit;
				}
			}
			mcbsp_cfg = 1;
		}

		if (params != NULL) {
			/* Configure McBSP internal buffer usage */
			/* this need to be done for playback and/or record */
			channels = params_channels(params);
			if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
				omap_mcbsp_set_rx_threshold(
					modem_rtd->cpu_dai->id, channels);
			else
				omap_mcbsp_set_tx_threshold(
					modem_rtd->cpu_dai->id, channels);
		}
	} else {
		mcbsp_cfg = 0;
	}

exit:
	return ret;
}

static int sdp4430_modem_hw_params(struct snd_pcm_substream *substream,
	struct snd_pcm_hw_params *params)
{
	int ret;

	ret = sdp4430_modem_mcbsp_configure(substream, params, 1);
	if (ret)
		printk(KERN_ERR "can't set modem cpu DAI configuration\n");

	return ret;
}

static int sdp4430_modem_hw_free(struct snd_pcm_substream *substream)
{
	int ret;

	ret = sdp4430_modem_mcbsp_configure(substream, NULL, 0);
	if (ret)
		printk(KERN_ERR "can't clear modem cpu DAI configuration\n");

	return ret;
}

static struct snd_soc_ops sdp4430_modem_ops = {
	.hw_params = sdp4430_modem_hw_params,
	.hw_free = sdp4430_modem_hw_free,
};

static int sdp4430_mcpdm_hw_params(struct snd_pcm_substream *substream,
	struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *codec_dai = rtd->codec_dai;
	int clk_id, freq, ret;

	if (twl6040_power_mode) {
		clk_id = TWL6040_SYSCLK_SEL_HPPLL;
		freq = 38400000;
	} else {
		clk_id = TWL6040_SYSCLK_SEL_LPPLL;
		freq = 32768;
	}

	/* set the codec mclk */
	ret = snd_soc_dai_set_sysclk(codec_dai, clk_id, freq,
				SND_SOC_CLOCK_IN);
	if (ret)
		printk(KERN_ERR "can't set codec system clock\n");

	return ret;
}

static struct snd_soc_ops sdp4430_mcpdm_ops = {
	.hw_params = sdp4430_mcpdm_hw_params,
};

static int sdp4430_mcbsp_hw_params(struct snd_pcm_substream *substream,
	struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	int ret = 0, channels = 0;
	unsigned int be_id, fmt;


        be_id = rtd->dai_link->be_id;

	if (be_id == OMAP_ABE_DAI_BT_VX) {
		if (machine_is_tuna())
			fmt = SND_SOC_DAIFMT_I2S |
				SND_SOC_DAIFMT_NB_NF |
				SND_SOC_DAIFMT_CBM_CFM;
		else
			fmt = SND_SOC_DAIFMT_DSP_B |
				SND_SOC_DAIFMT_NB_IF |
				SND_SOC_DAIFMT_CBM_CFM;
	} else {
		fmt = SND_SOC_DAIFMT_I2S |
			SND_SOC_DAIFMT_NB_NF |
			SND_SOC_DAIFMT_CBM_CFM;
	}

	ret = snd_soc_dai_set_fmt(cpu_dai, fmt);
	if (ret < 0) {
		printk(KERN_ERR "can't set cpu DAI configuration\n");
		return ret;
	}

	/*
	 * TODO: where does this clock come from (external source??) -
	 * do we need to enable it.
	 */
	/* Set McBSP clock to external */
	ret = snd_soc_dai_set_sysclk(cpu_dai, OMAP_MCBSP_SYSCLK_CLKS_FCLK,
				     32 * 96 * params_rate(params),
				     SND_SOC_CLOCK_IN);
	if (ret < 0)
		printk(KERN_ERR "can't set cpu system clock\n");

	ret = snd_soc_dai_set_clkdiv(cpu_dai, 0, 96);
	if (ret < 0)
		printk(KERN_ERR "can't set McBSP cpu DAI clkdiv\n");

	/*
	 * Configure McBSP internal buffer threshold
	 * for playback/record
	 */
	channels = params_channels(params);
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		omap_mcbsp_set_tx_threshold(cpu_dai->id, channels);
	else
		omap_mcbsp_set_rx_threshold(cpu_dai->id, channels);

	return ret;
}

static struct snd_soc_ops sdp4430_mcbsp_ops = {
	.hw_params = sdp4430_mcbsp_hw_params,
};

static int mcbsp_be_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
			struct snd_pcm_hw_params *params)
{
	struct snd_interval *channels = hw_param_interval(params,
                                       SNDRV_PCM_HW_PARAM_CHANNELS);
	unsigned int be_id = rtd->dai_link->be_id;

	if (be_id == OMAP_ABE_DAI_MM_FM)
		channels->min = 2;
	else if (be_id == OMAP_ABE_DAI_BT_VX)
		channels->min = 2;
	snd_mask_set(&params->masks[SNDRV_PCM_HW_PARAM_FORMAT -
	                            SNDRV_PCM_HW_PARAM_FIRST_MASK],
	                            SNDRV_PCM_FORMAT_S16_LE);
	return 0;
}

/* Headset jack */
static struct snd_soc_jack hs_jack;

/*Headset jack detection DAPM pins */
static struct snd_soc_jack_pin hs_jack_pins[] = {
	{
		.pin = "Headset Mic",
		.mask = SND_JACK_MICROPHONE,
	},
	{
		.pin = "Headset Stereophone",
		.mask = SND_JACK_HEADPHONE,
	},
};

static int sdp4430_get_power_mode(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = twl6040_power_mode;
	return 0;
}

static int sdp4430_set_power_mode(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	if (twl6040_power_mode == ucontrol->value.integer.value[0])
		return 0;

	twl6040_power_mode = ucontrol->value.integer.value[0];
	abe_dsp_set_power_mode(twl6040_power_mode);

	return 1;
}

static const char *power_texts[] = {"Low-Power", "High-Performance"};

static const struct soc_enum sdp4430_enum[] = {
	SOC_ENUM_SINGLE_EXT(2, power_texts),
};

static const struct snd_kcontrol_new sdp4430_controls[] = {
	SOC_ENUM_EXT("TWL6040 Power Mode", sdp4430_enum[0],
		sdp4430_get_power_mode, sdp4430_set_power_mode),
};

/* SDP4430 machine DAPM */
static const struct snd_soc_dapm_widget sdp4430_twl6040_dapm_widgets[] = {

	SND_SOC_DAPM_MIC("Ext Main Mic", NULL),
	SND_SOC_DAPM_MIC("Ext Sub Mic", NULL),
	SND_SOC_DAPM_MICBIAS_E("Ext Main Mic Bias", SND_SOC_NOPM, 0, 0,
				main_mic_bias_event,
				SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_PRE_PMD),
	SND_SOC_DAPM_MICBIAS_E("Ext Sub Mic Bias", SND_SOC_NOPM, 0, 0,
				sub_mic_bias_event,
				SND_SOC_DAPM_POST_PMU | SND_SOC_DAPM_PRE_PMD),
	SND_SOC_DAPM_SPK("Ext Spk", NULL),
	SND_SOC_DAPM_MIC("Headset Mic", NULL),
	SND_SOC_DAPM_HP("Headset Stereophone", NULL),
	SND_SOC_DAPM_SPK("Earphone Spk", NULL),
	SND_SOC_DAPM_INPUT("Aux/FM Stereo In"),
};

static const struct snd_soc_dapm_route audio_map[] = {
	/* External Mics: MAINMIC, SUBMIC with bias*/
	{"MAINMIC", NULL, "Ext Main Mic Bias"},
	{"SUBMIC", NULL, "Ext Sub Mic Bias"},
	{"Ext Main Mic Bias" , NULL, "Ext Main Mic"},
	{"Ext Sub Mic Bias" , NULL, "Ext Sub Mic"},

	/* External Speakers: HFL, HFR */
	{"Ext Spk", NULL, "HFL"},
	{"Ext Spk", NULL, "HFR"},

	/* Headset Mic: HSMIC with bias */
	{"HSMIC", NULL, "Headset Mic Bias"},
	{"Headset Mic Bias", NULL, "Headset Mic"},

	/* Headset Stereophone (Headphone): HSOL, HSOR */
	{"Headset Stereophone", NULL, "HSOL"},
	{"Headset Stereophone", NULL, "HSOR"},

	/* Earphone speaker */
	{"Earphone Spk", NULL, "EP"},

	/* Aux/FM Stereo In: AFML, AFMR */
	{"AFML", NULL, "Aux/FM Stereo In"},
	{"AFMR", NULL, "Aux/FM Stereo In"},
};

static int sdp4430_set_pdm_dl1_gains(struct snd_soc_dapm_context *dapm)
{
	int output, val;

	if (snd_soc_dapm_get_pin_power(dapm, "Earphone Spk")) {
		output = OMAP_ABE_DL1_EARPIECE;
	} else if (snd_soc_dapm_get_pin_power(dapm, "Headset Stereophone")) {
		val = snd_soc_read(twl6040_codec, TWL6040_REG_HSLCTL);
		if (val & TWL6040_HSDACMODEL)
			/* HSDACL in LP mode */
			output = OMAP_ABE_DL1_HEADSET_LP;
		else
			/* HSDACL in HP mode */
			output = OMAP_ABE_DL1_HEADSET_HP;
	} else {
		output = OMAP_ABE_DL1_NO_PDM;
	}

	return omap_abe_set_dl1_output(output);
}

static int sdp4430_mcpdm_twl6040_pre(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_codec *codec = rtd->codec;
	struct twl6040 *twl6040 = codec->control_data;
	int ret;

	/* enable external 32kHz clock */
	ret = regulator_enable(twl6040_clk32kreg);
	if (ret) {
		printk(KERN_ERR "failed to enable TWL6040 CLK32K\n");
		return ret;
	}

	/* disable internal 32kHz oscillator */
	twl6040_clear_bits(twl6040, TWL6040_REG_ACCCTL, TWL6040_CLK32KSEL);

	/* TWL6040 supplies McPDM PAD_CLKS */
	return twl6040_enable(twl6040);
}

static void sdp4430_mcpdm_twl6040_post(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_codec *codec = rtd->codec;
	struct twl6040 *twl6040 = codec->control_data;
	int ret;

	/* TWL6040 supplies McPDM PAD_CLKS */
	twl6040_disable(twl6040);

	/* enable internal 32kHz oscillator */
	twl6040_set_bits(twl6040, TWL6040_REG_ACCCTL, TWL6040_CLK32KSEL);

	/* disable external 32kHz clock */
	ret = regulator_disable(twl6040_clk32kreg);
	if (ret)
		printk(KERN_ERR "failed to disable TWL6040 CLK32K\n");
}

static int sdp4430_twl6040_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_codec *codec = rtd->codec;
	struct twl6040 *twl6040 = codec->control_data;
	struct snd_soc_dapm_context *dapm = &codec->dapm;
	int hsotrim, left_offset, right_offset, mode, ret;


	/* Add SDP4430 specific controls */
	ret = snd_soc_add_controls(codec, sdp4430_controls,
				ARRAY_SIZE(sdp4430_controls));
	if (ret)
		return ret;

	/* Add SDP4430 specific widgets */
	ret = snd_soc_dapm_new_controls(dapm, sdp4430_twl6040_dapm_widgets,
				ARRAY_SIZE(sdp4430_twl6040_dapm_widgets));
	if (ret)
		return ret;

	/* Set up SDP4430 specific audio path audio_map */
	snd_soc_dapm_add_routes(dapm, audio_map, ARRAY_SIZE(audio_map));

	/* SDP4430 connected pins */
	if (machine_is_tuna()) {
		snd_soc_dapm_enable_pin(dapm, "Ext Main Mic");
		snd_soc_dapm_enable_pin(dapm, "Ext Sub Mic");
	}
	snd_soc_dapm_enable_pin(dapm, "Ext Spk");
	snd_soc_dapm_enable_pin(dapm, "AFML");
	snd_soc_dapm_enable_pin(dapm, "AFMR");
	snd_soc_dapm_enable_pin(dapm, "Headset Mic");
	snd_soc_dapm_enable_pin(dapm, "Headset Stereophone");

	/* allow audio paths from the audio modem to run during suspend */
	if (machine_is_tuna()) {
		snd_soc_dapm_ignore_suspend(dapm, "Ext Main Mic");
		snd_soc_dapm_ignore_suspend(dapm, "Ext Sub Mic");
	}
	snd_soc_dapm_ignore_suspend(dapm, "Ext Spk");
	snd_soc_dapm_ignore_suspend(dapm, "AFML");
	snd_soc_dapm_ignore_suspend(dapm, "AFMR");
	snd_soc_dapm_ignore_suspend(dapm, "Headset Mic");
	snd_soc_dapm_ignore_suspend(dapm, "Headset Stereophone");

	ret = snd_soc_dapm_sync(dapm);
	if (ret)
		return ret;

	/* Headset jack detection */
	ret = snd_soc_jack_new(codec, "Headset Jack",
				SND_JACK_HEADSET, &hs_jack);
	if (ret)
		return ret;

	ret = snd_soc_jack_add_pins(&hs_jack, ARRAY_SIZE(hs_jack_pins),
				hs_jack_pins);

	if (machine_is_omap_4430sdp())
		twl6040_hs_jack_detect(codec, &hs_jack, SND_JACK_HEADSET);
	else
		snd_soc_jack_report(&hs_jack, SND_JACK_HEADSET, SND_JACK_HEADSET);

	/* DC offset cancellation computation */
	hsotrim = snd_soc_read(codec, TWL6040_REG_HSOTRIM);
	right_offset = (hsotrim & TWL6040_HSRO) >> TWL6040_HSRO_OFFSET;
	left_offset = hsotrim & TWL6040_HSLO;

	if (twl6040_get_icrev(twl6040) < TWL6040_REV_1_3)
		/* For ES under ES_1.3 HS step is 2 mV */
		mode = 2;
	else
		/* For ES_1.3 HS step is 1 mV */
		mode = 1;

	abe_dsp_set_hs_offset(left_offset, right_offset, mode);

	/* don't wait before switching of HS power */
	rtd->pmdown_time = 0;

	return ret;
}

static int sdp4430_twl6040_dl2_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_codec *codec = rtd->codec;
	int hfotrim, left_offset, right_offset;

	/* DC offset cancellation computation */
	hfotrim = snd_soc_read(codec, TWL6040_REG_HFOTRIM);
	right_offset = (hfotrim & TWL6040_HFRO) >> TWL6040_HFRO_OFFSET;
	left_offset = hfotrim & TWL6040_HFLO;

	abe_dsp_set_hf_offset(left_offset, right_offset);

	/* don't wait before switching of HF power */
	rtd->pmdown_time = 0;

	return 0;
}

static int sdp4430_twl6040_fe_init(struct snd_soc_pcm_runtime *rtd)
{

	/* don't wait before switching of FE power */
	rtd->pmdown_time = 0;

	return 0;
}

static int sdp4430_bt_init(struct snd_soc_pcm_runtime *rtd)
{

	/* don't wait before switching of BT power */
	rtd->pmdown_time = 0;

	return 0;
}

static int sdp4430_spdif_init(struct snd_soc_pcm_runtime *rtd)
{
	rtd->pmdown_time = 0;
	return 0;
}

static int sdp4430_stream_event(struct snd_soc_dapm_context *dapm)
{
	/*
	 * set DL1 gains dynamically according to the active output
	 * (Headset, Earpiece) and HSDAC power mode
	 */
	return sdp4430_set_pdm_dl1_gains(dapm);
}

/* TODO: make this a separate BT CODEC driver or DUMMY */
static struct snd_soc_dai_driver dai[] = {
{
	.name = "Bluetooth",
	.playback = {
		.stream_name = "BT Playback",
		.channels_min = 1,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_8000 | SNDRV_PCM_RATE_16000 |
					SNDRV_PCM_RATE_48000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE,
	},
	.capture = {
		.stream_name = "BT Capture",
		.channels_min = 1,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_8000 | SNDRV_PCM_RATE_16000 |
					SNDRV_PCM_RATE_48000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE,
	},
},
/* TODO: make this a separate FM CODEC driver or DUMMY */
{
	.name = "FM Digital",
	.playback = {
		.stream_name = "FM Playback",
		.channels_min = 1,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_48000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE,
	},
	.capture = {
		.stream_name = "FM Capture",
		.channels_min = 1,
		.channels_max = 2,
		.rates = SNDRV_PCM_RATE_48000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE,
	},
},
{
	.name = "HDMI",
	.playback = {
		.stream_name = "HDMI Playback",
		.channels_min = 2,
		.channels_max = 8,
		.rates = SNDRV_PCM_RATE_32000 | SNDRV_PCM_RATE_44100 |
				SNDRV_PCM_RATE_48000,
		.formats = SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S24_LE,
	},
},
};

struct snd_soc_dsp_link fe_media = {
	.playback	= true,
	.capture	= true,
	.trigger =
		{SND_SOC_DSP_TRIGGER_BESPOKE, SND_SOC_DSP_TRIGGER_BESPOKE},
};

struct snd_soc_dsp_link fe_media_capture = {
	.capture	= true,
	.trigger =
		{SND_SOC_DSP_TRIGGER_BESPOKE, SND_SOC_DSP_TRIGGER_BESPOKE},
};

struct snd_soc_dsp_link fe_tones = {
	.playback	= true,
	.trigger =
		{SND_SOC_DSP_TRIGGER_BESPOKE, SND_SOC_DSP_TRIGGER_BESPOKE},
};

struct snd_soc_dsp_link fe_vib = {
	.playback	= true,
	.trigger =
		{SND_SOC_DSP_TRIGGER_BESPOKE, SND_SOC_DSP_TRIGGER_BESPOKE},
};

struct snd_soc_dsp_link fe_modem = {
	.playback	= true,
	.capture	= true,
	.trigger =
		{SND_SOC_DSP_TRIGGER_BESPOKE, SND_SOC_DSP_TRIGGER_BESPOKE},
};

struct snd_soc_dsp_link fe_lp_media = {
	.playback	= true,
	.trigger =
		{SND_SOC_DSP_TRIGGER_BESPOKE, SND_SOC_DSP_TRIGGER_BESPOKE},
};
/* Digital audio interface glue - connects codec <--> CPU */
static struct snd_soc_dai_link sdp4430_dai[] = {

/*
 * Frontend DAIs - i.e. userspace visible interfaces (ALSA PCMs)
 */

	{
		.name = "SDP4430 Media",
		.stream_name = "Multimedia",

		/* ABE components - MM-UL & MM_DL */
		.cpu_dai_name = "MultiMedia1",
		.platform_name = "omap-pcm-audio",

		.dynamic = 1, /* BE is dynamic */
		.init = sdp4430_twl6040_fe_init,
		.dsp_link = &fe_media,
	},
	{
		.name = "SDP4430 Media Capture",
		.stream_name = "Multimedia Capture",

		/* ABE components - MM-UL2 */
		.cpu_dai_name = "MultiMedia2",
		.platform_name = "omap-pcm-audio",

		.dynamic = 1, /* BE is dynamic */
		.dsp_link = &fe_media_capture,
	},
	{
		.name = "SDP4430 Voice",
		.stream_name = "Voice",

		/* ABE components - VX-UL & VX-DL */
		.cpu_dai_name = "Voice",
		.platform_name = "omap-pcm-audio",

		.dynamic = 1, /* BE is dynamic */
		.dsp_link = &fe_media,
		.no_host_mode = SND_SOC_DAI_LINK_OPT_HOST,
	},
	{
		.name = "SDP4430 Tones Playback",
		.stream_name = "Tone Playback",

		/* ABE components - TONES_DL */
		.cpu_dai_name = "Tones",
		.platform_name = "omap-pcm-audio",

		.dynamic = 1, /* BE is dynamic */
		.dsp_link = &fe_tones,
	},
	{
		.name = "SDP4430 Vibra Playback",
		.stream_name = "VIB-DL",

		/* ABE components - DMIC UL 2 */
		.cpu_dai_name = "Vibra",
		.platform_name = "omap-pcm-audio",

		.dynamic = 1, /* BE is dynamic */
		.dsp_link = &fe_vib,
	},
	{
		.name = "SDP4430 MODEM",
		.stream_name = "MODEM",

		/* ABE components - MODEM <-> McBSP2 */
		.cpu_dai_name = "MODEM",
		.platform_name = "aess",

		.dynamic = 1, /* BE is dynamic */
		.init = sdp4430_twl6040_fe_init,
		.dsp_link = &fe_modem,
		.ops = &sdp4430_modem_ops,
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		.ignore_suspend = 1,
	},
	{
		.name = "SDP4430 Media LP",
		.stream_name = "Multimedia",

		/* ABE components - MM-DL (mmap) */
		.cpu_dai_name = "MultiMedia1 LP",
		.platform_name = "aess",

		.dynamic = 1, /* BE is dynamic */
		.dsp_link = &fe_lp_media,
	},
	{
		.name = "Legacy McBSP",
		.stream_name = "Multimedia",

		/* ABE components - MCBSP2 - MM-EXT */
		.cpu_dai_name = "omap-mcbsp-dai.1",
		.platform_name = "omap-pcm-audio",

		/* FM */
		.codec_dai_name = "FM Digital",

		.no_codec = 1, /* TODO: have a dummy CODEC */
		.ops = &sdp4430_mcbsp_ops,
		.ignore_suspend = 1,
	},
	{
		.name = "Legacy McPDM",
		.stream_name = "Headset Playback",

		/* ABE components - DL1 */
		.cpu_dai_name = "mcpdm-dl",
		.platform_name = "omap-pcm-audio",

		/* Phoenix - DL1 DAC */
		.codec_dai_name =  "twl6040-dl1",
		.codec_name = "twl6040-codec",

		.pre = sdp4430_mcpdm_twl6040_pre,
		.post = sdp4430_mcpdm_twl6040_post,
		.ops = &sdp4430_mcpdm_ops,
		.ignore_suspend = 1,
	},
	{
		.name = "SPDIF",
		.stream_name = "SPDIF",
		.cpu_dai_name = "omap-mcasp-dai",
		.codec_dai_name = "dit-hifi",	/* dummy s/pdif transciever
						 * driver */
		.platform_name = "omap-pcm-audio",
		.ignore_suspend = 1,
		.no_codec = 1,
		.init = sdp4430_spdif_init,
	},

/*
 * Backend DAIs - i.e. dynamically matched interfaces, invisible to userspace.
 * Matched to above interfaces at runtime, based upon use case.
 */

	{
		.name = OMAP_ABE_BE_PDM_DL1,
		.stream_name = "HS Playback",

		/* ABE components - DL1 */
		.cpu_dai_name = "mcpdm-dl1",
		.platform_name = "aess",

		/* Phoenix - DL1 DAC */
		.codec_dai_name =  "twl6040-dl1",
		.codec_name = "twl6040-codec",

		.no_pcm = 1, /* don't create ALSA pcm for this */
		.init = sdp4430_twl6040_init,
		.pre = sdp4430_mcpdm_twl6040_pre,
		.post = sdp4430_mcpdm_twl6040_post,
		.ops = &sdp4430_mcpdm_ops,
		.be_id = OMAP_ABE_DAI_PDM_DL1,
		.ignore_suspend = 1,
	},
	{
		.name = OMAP_ABE_BE_PDM_UL1,
		.stream_name = "Analog Capture",

		/* ABE components - UL1 */
		.cpu_dai_name = "mcpdm-ul1",
		.platform_name = "aess",

		/* Phoenix - UL ADC */
		.codec_dai_name =  "twl6040-ul",
		.codec_name = "twl6040-codec",

		.no_pcm = 1, /* don't create ALSA pcm for this */
		.ops = &sdp4430_mcpdm_ops,
		.pre = sdp4430_mcpdm_twl6040_pre,
		.post = sdp4430_mcpdm_twl6040_post,
		.be_id = OMAP_ABE_DAI_PDM_UL,
		.ignore_suspend = 1,
	},
	{
		.name = OMAP_ABE_BE_PDM_DL2,
		.stream_name = "HF Playback",

		/* ABE components - DL2 */
		.cpu_dai_name = "mcpdm-dl2",
		.platform_name = "aess",

		/* Phoenix - DL2 DAC */
		.codec_dai_name =  "twl6040-dl2",
		.codec_name = "twl6040-codec",

		.no_pcm = 1, /* don't create ALSA pcm for this */
		.init = sdp4430_twl6040_dl2_init,
		.pre = sdp4430_mcpdm_twl6040_pre,
		.post = sdp4430_mcpdm_twl6040_post,
		.ops = &sdp4430_mcpdm_ops,
		.be_id = OMAP_ABE_DAI_PDM_DL2,
		.ignore_suspend = 1,
	},
	{
		.name = OMAP_ABE_BE_PDM_VIB,
		.stream_name = "Vibra",

		/* ABE components - VIB1 DL */
		.cpu_dai_name = "mcpdm-vib",
		.platform_name = "aess",

		/* Phoenix - PDM to PWM */
		.codec_dai_name =  "twl6040-vib",
		.codec_name = "twl6040-codec",

		.no_pcm = 1, /* don't create ALSA pcm for this */
		.pre = sdp4430_mcpdm_twl6040_pre,
		.post = sdp4430_mcpdm_twl6040_post,
		.ops = &sdp4430_mcpdm_ops,
		.be_id = OMAP_ABE_DAI_PDM_VIB,
	},
	{
		.name = OMAP_ABE_BE_BT_VX_UL,
		.stream_name = "BT Capture",

		/* ABE components - MCBSP1 - BT-VX */
		.cpu_dai_name = "omap-mcbsp-dai.0",
		.platform_name = "aess",

		/* Bluetooth */
		.codec_dai_name = "Bluetooth",

		.no_pcm = 1, /* don't create ALSA pcm for this */
		.no_codec = 1, /* TODO: have a dummy CODEC */
		.be_hw_params_fixup = mcbsp_be_hw_params_fixup,
		.ops = &sdp4430_mcbsp_ops,
		.be_id = OMAP_ABE_DAI_BT_VX,
		.ignore_suspend = 1,
	},
	{
		.name = OMAP_ABE_BE_BT_VX_DL,
		.stream_name = "BT Playback",

		/* ABE components - MCBSP1 - BT-VX */
		.cpu_dai_name = "omap-mcbsp-dai.0",
		.platform_name = "aess",

		/* Bluetooth */
		.codec_dai_name = "Bluetooth",

		.no_pcm = 1, /* don't create ALSA pcm for this */
		.no_codec = 1, /* TODO: have a dummy CODEC */
		.init = sdp4430_bt_init,
		.be_hw_params_fixup = mcbsp_be_hw_params_fixup,
		.ops = &sdp4430_mcbsp_ops,
		.be_id = OMAP_ABE_DAI_BT_VX,
		.ignore_suspend = 1,
	},
	{
		.name = OMAP_ABE_BE_MM_EXT0,
		.stream_name = "FM",

		/* ABE components - MCBSP2 - MM-EXT */
		.cpu_dai_name = "omap-mcbsp-dai.1",
		.platform_name = "aess",

		/* FM */
		.codec_dai_name = "FM Digital",

		.no_pcm = 1, /* don't create ALSA pcm for this */
		.no_codec = 1, /* TODO: have a dummy CODEC */
		.be_hw_params_fixup = mcbsp_be_hw_params_fixup,
		.ops = &sdp4430_mcbsp_ops,
		.be_id = OMAP_ABE_DAI_MM_FM,
	},
	{
		.name = OMAP_ABE_BE_MM_EXT1,
		.stream_name = "MODEM",

		/* ABE components - MCBSP2 - MM-EXT */
		.cpu_dai_name = "omap-mcbsp-dai.1",
		.platform_name = "aess",

		/* MODEM */
		.codec_dai_name = "MODEM",

		.no_pcm = 1, /* don't create ALSA pcm for this */
		.no_codec = 1, /* TODO: have a dummy CODEC */
		.be_hw_params_fixup = mcbsp_be_hw_params_fixup,
		.ops = &sdp4430_mcbsp_ops,
		.be_id = OMAP_ABE_DAI_MODEM,
		.ignore_suspend = 1,
	},
};

/* Audio machine driver */
static struct snd_soc_card snd_soc_sdp4430 = {
	.driver_name = "OMAP4",
	.long_name = "TI OMAP4 Board",
	.dai_link = sdp4430_dai,
	.num_links = ARRAY_SIZE(sdp4430_dai),
	.stream_event = sdp4430_stream_event,
};

static struct platform_device *sdp4430_snd_device;
struct i2c_adapter *adapter;

static int __init sdp4430_soc_init(void)
{
	int ret;

	if (!machine_is_omap_4430sdp() && !machine_is_omap4_panda() &&
			!machine_is_tuna()) {
		pr_debug("Not SDP4430, PandaBoard or Tuna!\n");
		return -ENODEV;
	}
	printk(KERN_INFO "SDP4430 SoC init\n");

	if (machine_is_tuna()) {
		ret = gpio_request(TUNA_MAIN_MIC_GPIO, "MAIN_MICBIAS_EN");
		if (ret)
			goto mainmic_gpio_err;

		gpio_direction_output(TUNA_MAIN_MIC_GPIO, 0);

		ret = gpio_request(TUNA_SUB_MIC_GPIO, "SUB_MICBIAS_EN");
		if (ret)
			goto submic_gpio_err;
		gpio_direction_output(TUNA_SUB_MIC_GPIO, 0);
	}

	if (machine_is_omap_4430sdp())
		snd_soc_sdp4430.name = "SDP4430";
	else if (machine_is_omap4_panda())
		snd_soc_sdp4430.name = "Panda";
	else if (machine_is_tuna())
		snd_soc_sdp4430.name = "Tuna";

	twl6040_clk32kreg = regulator_get(NULL, "twl6040_clk32k");
	if (IS_ERR(twl6040_clk32kreg)) {
		ret = PTR_ERR(twl6040_clk32kreg);
		printk(KERN_ERR "failed to get CLK32K %d\n", ret);
		goto clk32kreg_err;
	}

	/* enable external 32kHz clock during TWL6040/McPDM probe */
	ret = regulator_enable(twl6040_clk32kreg);
	if (ret) {
		printk(KERN_ERR "failed to enable TWL6040 CLK32K\n");
		goto clk32kreg_ena_err;
	}

	sdp4430_snd_device = platform_device_alloc("soc-audio", -1);
	if (!sdp4430_snd_device) {
		printk(KERN_ERR "Platform device allocation failed\n");
		ret = -ENOMEM;
		goto device_err;
	}

	ret = snd_soc_register_dais(&sdp4430_snd_device->dev, dai, ARRAY_SIZE(dai));
	if (ret < 0)
		goto err;
	platform_set_drvdata(sdp4430_snd_device, &snd_soc_sdp4430);

	ret = platform_device_add(sdp4430_snd_device);
	if (ret)
		goto err;

	regulator_disable(twl6040_clk32kreg);

	twl6040_codec = snd_soc_card_get_codec(&snd_soc_sdp4430,
					"twl6040-codec");

	return 0;

err:
	printk(KERN_ERR "Unable to add platform device\n");
	platform_device_put(sdp4430_snd_device);
device_err:
	regulator_disable(twl6040_clk32kreg);
clk32kreg_ena_err:
	regulator_put(twl6040_clk32kreg);
clk32kreg_err:
	if (machine_is_tuna())
		gpio_free(TUNA_SUB_MIC_GPIO);
submic_gpio_err:
	if (machine_is_tuna())
		gpio_free(TUNA_MAIN_MIC_GPIO);
mainmic_gpio_err:
	return ret;
}
module_init(sdp4430_soc_init);

static void __exit sdp4430_soc_exit(void)
{
	if (machine_is_tuna()) {
		gpio_free(TUNA_SUB_MIC_GPIO);
		gpio_free(TUNA_MAIN_MIC_GPIO);
	}
	regulator_put(twl6040_clk32kreg);
	platform_device_unregister(sdp4430_snd_device);
}
module_exit(sdp4430_soc_exit);

MODULE_AUTHOR("Misael Lopez Cruz <x0052729@ti.com>");
MODULE_DESCRIPTION("ALSA SoC SDP4430");
MODULE_LICENSE("GPL");

