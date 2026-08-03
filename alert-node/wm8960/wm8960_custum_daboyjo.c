#include <linux/init.h>
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/mod_devicetable.h>
#include <sound/soc.h>
#include <sound/pcm_params.h>


//  wm8960 regester map 

#define WM8960_LOUT1V           0x02
#define WM8960_ROUT1V	        0x03
#define WM8960_CLOKING          0x04
#define WM8960_ADC_DAC_CTR1     0x05
#define WM8960_AUDIO_INTERFACE  0x07
#define WM8960_L2MO             0x26
#define WM8960_R2MO             0x27
#define WM8960_LSPKV			0x28
#define WM8960_RSPKV			0x29
#define WM8960_POWER_MGMT1		0x19
#define WM8960_POWER_MGMT2		0x1A
#define WM8960_POWER_MGMT3		0x2F
#define WM8960_LOUTMIX			0x22
#define WM8960_ROUTMIX          0x25
#define WM8960_CLASSD           0x31
#define WM8960_RESET            0x0F
//#define WM8960_

static int veda_wm8960_write(struct i2c_client *client, uint8_t addr, uint8_t data_first, uint8_t data, char *messge)
{
    
    uint8_t buf[2];
    int check;
    
    buf[0] = addr << 1;
    buf[0] = buf[0] | data_first;
    buf[1] = data;

    check = i2c_master_send(client, buf, 2);

    if( check == 2){
        dev_info(&client->dev, "wm8960 %s success\n",messge);
        return 0;
    }else{
        dev_info(&client->dev, "wm8960 %s fail\n",messge);
        return check;
    }
}

static int veda_wm8960_starup(struct snd_pcm_substream *substream,struct snd_soc_dai *dai) {
    struct snd_pcm_runtime *runtime = substream->runtime;

    runtime->hw.info |= SNDRV_PCM_INFO_PAUSE;
    return 0;
}

static int veda_wm_hw_params(struct snd_pcm_substream *substream, struct snd_pcm_hw_params *params,struct snd_soc_dai *dai)
{
    int ret;
    struct snd_soc_component *component = dai->component;

    // i2c_client 추출 코드 
    struct i2c_client *client = to_i2c_client(component->dev);

    int rate = params_rate(params);
    int width = params_width(params);
    unsigned int def = 0;
    unsigned int width_def = 0;
    uint8_t dacdiv = 0;
    uint8_t sysclkdiv = 0;
    

    switch(width){
        case 16:
            width_def = 0x00;
            break;
        case 20:
            width_def = 0x01;
            break;
        case 24:
            width_def = 0x02;
            break;
        case 32:
            width_def = 0x03;
            break;

        default:
            return -EINVAL;
    };

    switch(rate) {
        case 8000:
            dacdiv = 0x06;
            sysclkdiv = 0x00;
            break;
        case 16000:
            dacdiv = 0x03;
            sysclkdiv = 0x00;
            break;
        case 44100:
        case 48000:
            dacdiv = 0x00;
            sysclkdiv = 0x00;
            break;
        default:
           return -EINVAL;
    };
    
    def = snd_soc_component_read(component, 0x07);
    if(((def>>2) & 3) != width_def){
        def = def & ~(0x3<<2);
        def = def | (width_def <<2);
        ret = veda_wm8960_write(client,WM8960_AUDIO_INTERFACE,(uint8_t)(def>>8),(uint8_t)def,"audio inerface setting");
        if(ret<0) return ret;
    }
    def = snd_soc_component_read(component, 0x04);
    if((((def>>3) & 7) != dacdiv) ||(((def >> 1) & 3) != sysclkdiv)){
        def = def & ~(0x1F<<1);
        def = def | (dacdiv <<3) | (sysclkdiv <<1);
        ret = veda_wm8960_write(client,WM8960_CLOKING,(uint8_t)(def>>8),(uint8_t)def,"clocking setting");
        if(ret<0) return ret;
    }



    return 0;
}

static int veda_wm8960_trigger(struct snd_pcm_substream *substream, int cmd, struct snd_soc_dai *dai)
{
    struct snd_soc_component  *component = dai->component;

    switch(cmd) {
        //case SNDRV_PCM_TRIGGER_
        case SNDRV_PCM_TRIGGER_START:
        case SNDRV_PCM_TRIGGER_RESUME:
        case SNDRV_PCM_TRIGGER_PAUSE_RELEASE: // pause 해제 -> 소리출력 레지스터 제어 넣기  
            
            return 0;
            break;
        
        case SNDRV_PCM_TRIGGER_STOP:
        case SNDRV_PCM_TRIGGER_SUSPEND:
        case SNDRV_PCM_TRIGGER_PAUSE_PUSH:
            return 0;
            break;

        default:
            return -EINVAL;
    }
    return 0;
}

static const struct snd_soc_dai_ops veda_wm8960_dai_ops = {
    .startup   = veda_wm8960_starup,
    .hw_params = veda_wm_hw_params,
    .trigger   = veda_wm8960_trigger,
};


static const struct snd_soc_dapm_widget veda_wm8960_dapm_widgets[] = {
    SND_SOC_DAPM_OUTPUT("LOUT1"),
    SND_SOC_DAPM_OUTPUT("ROUT1"),
    SND_SOC_DAPM_OUTPUT("SPKLOUT"),
    SND_SOC_DAPM_OUTPUT("SPKROUT"),
    SND_SOC_DAPM_INPUT("LINPUT1"),
    SND_SOC_DAPM_INPUT("RINPUT1"),

    SND_SOC_DAPM_MIXER("Left Output Mixer", SND_SOC_NOPM,0,0,NULL,0),
    SND_SOC_DAPM_MIXER("Right Output Mixer", SND_SOC_NOPM,0,0,NULL,0),

};

static const struct snd_soc_dapm_route veda_wm8960_dapm_routes[] = {
    { "LOUT1", NULL, "Left Output Mixer" },
    { "ROUT1", NULL, "Right Output Mixer" },

};

    
static const struct snd_kcontrol_new veda_wm8960_controls[] = {
    SOC_DOUBLE_R("Headphone Playback Volume",WM8960_LOUT1V,WM8960_ROUT1V, 0, 127,0),
    
    SOC_DOUBLE_R("Speaker Playback Volume", WM8960_LSPKV ,WM8960_R2MO, 0, 127, 0),
    SOC_DOUBLE_R("Mono Ouo Mix", WM8960_LOUTMIX,WM8960_ROUTMIX, 7, 1, 0),

    SOC_SINGLE("Playback Switch",WM8960_ADC_DAC_CTR1,3,1,1),
};


struct snd_soc_component_driver veda_wm8960_component_driver = {
    .name = "veda-wm8960",
    .dapm_widgets = veda_wm8960_dapm_widgets,
    .num_dapm_widgets = ARRAY_SIZE(veda_wm8960_dapm_widgets),
    .dapm_routes = veda_wm8960_dapm_routes,
    .num_dapm_routes = ARRAY_SIZE(veda_wm8960_dapm_routes),
    .controls = veda_wm8960_controls,
    .num_controls = ARRAY_SIZE(veda_wm8960_controls),
};

struct snd_soc_dai_driver veda_wm8960_dai_driver = {
    .name = "veda-wm8960-dai",
    .playback = {
        .stream_name = "HiFi Playback",
        .channels_min = 1,
        .channels_max = 2,
        .rates = SNDRV_PCM_RATE_8000_48000,
        .formats = SNDRV_PCM_FMTBIT_S16_LE,},
    .ops = &veda_wm8960_dai_ops,
};



static const struct reg_default wm8960_reg_defaults[] = {
    {WM8960_LOUT1V, 0x179 },
    {WM8960_ROUT1V, 0x179 },
    {WM8960_CLOKING, 0x000 },
    {WM8960_ADC_DAC_CTR1, 0x008 },
    {WM8960_AUDIO_INTERFACE, 0x002 },
    {WM8960_L2MO, 0x000 },
    {WM8960_R2MO, 0x000 },
    {WM8960_LSPKV, 0x179 },
    {WM8960_RSPKV, 0x179 },
    {WM8960_LOUTMIX, 0x1D0},
    {WM8960_ROUTMIX, 0x1D0},
};
    

struct regmap_config wm8960_regmap = {
    .reg_bits = 7,
    .val_bits = 9,
    .max_register = 0x37,

    .cache_type = REGCACHE_RBTREE,
    .reg_defaults = wm8960_reg_defaults,
    .num_reg_defaults = ARRAY_SIZE(wm8960_reg_defaults),
    .read_flag_mask = 0,
};

static int veda_wm8960_probe(struct i2c_client *client,const struct i2c_device_id *id)
{
    int ret;
    struct regmap *regmap;
    dev_info(&client->dev, "veda_wm8960 probe success");
    dev_info(&client->dev, "I2C 장치 주소 0x%02x\n", client->addr);

    regmap = devm_regmap_init_i2c(client,&wm8960_regmap);

    if(IS_ERR(regmap)) {
        dev_err(&client->dev, "fail to allocate regmap\n");
        return PTR_ERR(regmap);
    }

    // reset 
    // 0Fh 0 0000 0000
    ret = veda_wm8960_write(client,WM8960_RESET,0,0x00,"reset");
    if(ret<0) return ret;

    // clocking 
    // 04h 0 0000 0000 
    ret = veda_wm8960_write(client,WM8960_CLOKING,0,0x00,"clock setting");
    if(ret<0) return ret;

    //audio interface
    // 07h 0 0000 0010 
    ret = veda_wm8960_write(client,WM8960_AUDIO_INTERFACE,0,0x02,"audio inerface setting");
    if(ret<0) return ret;

    // power mgmt
    // 19h 0 1100 0000
    ret = veda_wm8960_write(client,WM8960_POWER_MGMT1,0,0xC0,"power up");
    if(ret<0) return ret;

    // power mgmt 2
    // 1Ah 1 1111 1000
    ret = veda_wm8960_write(client,WM8960_POWER_MGMT2,1,0xF8,"power(lout rout spkl spkr) up");
    if(ret<0) return ret;


    // power mgmt 3
    // 2Fh 0 0000 1100
    ret = veda_wm8960_write(client,WM8960_POWER_MGMT3,0,0x0C,"power(lomix romix) up");
    if(ret<0) return ret;

    // Left Output Mixer Routing 
    // 22h 1 1101 0000
    ret = veda_wm8960_write(client,WM8960_LOUTMIX,1,0xD0,"Left Output Mixer Routing");
    if(ret<0) return ret;

    // Right Output Mixer Routing 
    // 25h 1 1101 0000
    ret = veda_wm8960_write(client,WM8960_ROUTMIX,1,0xD0,"Right Output Mixer Routing");
    if(ret<0) return ret;
   
    // no mute
    ret = regmap_write(regmap, WM8960_ADC_DAC_CTR1, 0x00);
    if (ret < 0) return ret;

    // class D control (speaker) 
    // 0x31
    // 7:6 
    // 00 off 01 left 10 right 11 left and right
    // 0 0111 0111 (지금은 left만 나중에 양쪽으로 고칠 예정)
    ret = veda_wm8960_write(client,WM8960_CLASSD,0,0x77,"Speaker Output enabled");
    if(ret<0) return ret;



    devm_snd_soc_register_component(&client->dev,&veda_wm8960_component_driver,&veda_wm8960_dai_driver,1);
    
    ret = veda_wm8960_write(client, WM8960_LOUT1V, 1, 0x79, "Headphone L Vol");
    if(ret<0) return ret;
    ret = veda_wm8960_write(client, WM8960_ROUT1V, 1, 0x79, "Headphone R Vol");
    if(ret<0) return ret;
    ret = veda_wm8960_write(client, WM8960_LSPKV, 1, 0x79, "Speaker L Vol");
    if(ret<0) return ret;
    ret = veda_wm8960_write(client, WM8960_RSPKV, 1, 0x79, "Speaker R Vol");
    if(ret<0) return ret;



    return 0;
}

static void veda_wm8960_remove(struct i2c_client *client)
{
    dev_info(&client->dev,"veda_wm8960 remove");
}


static const struct of_device_id veda_wm8960_of_match[] = {
    { .compatible = "veda,wm8960_daboyjo", },
    { }
};

MODULE_DEVICE_TABLE(of,veda_wm8960_of_match);

static struct i2c_driver veda_wm8960_driver = {
    .driver = {
        .name = "veda_wm8960_codec_driver",
        .of_match_table = veda_wm8960_of_match,
    },
    .probe = veda_wm8960_probe,
    .remove = veda_wm8960_remove,
};

module_i2c_driver(veda_wm8960_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Veda Project daboyjo");
MODULE_DESCRIPTION("Custom WM8960 Audio Codec Driver");
