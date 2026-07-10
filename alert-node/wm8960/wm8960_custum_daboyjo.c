#include <linux/init.h>
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/mod_devicetable.h>
#include <sound/soc.h>




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
    SOC_DOUBLE_R("Headphone Playback Volume",0x02,0x03, 0, 127,0),
    
    SOC_DOUBLE_R("Speaker Playback Volume", 0x28,0x29, 0, 127, 0),

    SOC_SINGLE("Playback Switch",0x05,3,1,1),
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
};

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


static const struct reg_default wm8960_reg_defaults[] = {
    {0x02, 0x000 },
    {0x03, 0x000 },
    {0x05, 0x008 },
    {0x28, 0x000 },
    {0x29, 0x000 },
};
    

struct regmap_config wm8960_regmap = {
    .reg_bits = 7,
    .val_bits = 9,
    .max_register = 0x38,

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
    ret = veda_wm8960_write(client,0x0F,0,0x00,"reset");
    if(ret<0) return ret;

    // clocking 
    // 04h 0 0000 0000 
    ret = veda_wm8960_write(client,0x04,0,0x00,"clock setting");
    if(ret<0) return ret;

    //audio interface
    // 07h 0 0000 0010 
    ret = veda_wm8960_write(client,0x07,0,0x02,"audio inerface setting");
    if(ret<0) return ret;

    // power mgmt
    // 19h 0 1100 0000
    ret = veda_wm8960_write(client,0x19,0,0xC0,"power up");
    if(ret<0) return ret;

    // power mgmt 2
    // 1Ah 1 1111 1000
    ret = veda_wm8960_write(client,0x1A,1,0xF8,"power(lout rout spkl spkr) up");
    if(ret<0) return ret;


    // power mgmt 3
    // 2Fh 0 0000 1100
    ret = veda_wm8960_write(client,0x2F,0,0x0C,"power(lomix romix) up");
    if(ret<0) return ret;

    // Left Output Mixer Routing 
    // 22h 0 1101 0000
    ret = veda_wm8960_write(client,0x22,0,0xD0,"Left Output Mixer Routing");
    if(ret<0) return ret;

    // Right Output Mixer Routing 
    // 25h 0 1101 0000
    ret = veda_wm8960_write(client,0x25,0,0xD0,"Right Output Mixer Routing");
    if(ret<0) return ret;
   
    // no mute
    ret = regmap_write(regmap, 0x05, 0x00);
    if (ret < 0) return ret;

    devm_snd_soc_register_component(&client->dev,&veda_wm8960_component_driver,&veda_wm8960_dai_driver,1);
    




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
