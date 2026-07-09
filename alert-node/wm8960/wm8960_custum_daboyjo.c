#include <linux/init.h>
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/mod_devicetable.h>

static int veda_wm8960_write(struct i2c_client *client, uint8_t addr, uint8_t data_first, uint8_t data, char *messge)
{
    
    uint8_t buf[2];
    
    buf[0] = addr << 1;
    buf[0] = buf[0] | data_first;
    buf[1] = data;

    int check = i2c_master_send(client, buf, 2);

    if( check == 2){
        dev_info(&client->dev, "wm8960 %s success\n",messge);
        return 0;
    }else{
        dev_info(&client->dev, "wm8960 %s fail\n",messge);
        return check;
    }
}


static int veda_wm8960_probe(struct i2c_client *client,const struct i2c_device_id *id)
{
    dev_info(&client->dev, "veda_wm8960 probe success");
    dev_info(&client->dev, "I2C 장치 주소 0x%02x\n", client->addr);

    /*TOTO:
    

    */

    int ret;
    // reset 
    // 0Fh 0 0000 0000
    ret = veda_wm8960_write(client,0x0F,0,0x00,"reset");
    if(ret<0) return ret;

    // power mgmt
    // 19h 0 1100 0000
    ret = veda_wm8960_write(client,0x19,0,0xC0,"power up");
    if(ret<0) return ret;

    //audio interface
    // 07h 0 0000 0010 
    ret = veda_wm8960_write(client,0x07,0,0x02,"audio inerface setting");
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
