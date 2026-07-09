#include <linux/init.h>
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/mod_devicetable.h>


static int veda_wm8960_probe(struct i2c_client *client,const struct i2c_device_id *id)
{
    dev_info(&client->dev, "veda_wm8960 probe success");
    dev_info(&client->dev, "I2C 장치 주소 0x%02x\n", client->addr);

    /*TOTO:

    */

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
