#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/gpio.h>
#include <linux/timer.h>

#include "hdmitx.h"

#define AUDIO_MUTE_ON	0
#define AUDIO_MUTE_OFF	1

struct device_node *hdmitx_dev_node;
static int audio_mute_gpio = -1;
struct timer_list *mute_gpio_timer;


void set_mute_gpio(int gpio_number, int gpio_value)
{
	if (gpio_request(gpio_number, hdmitx_dev_node->name)) {
		HDMI_ERROR("[%s] Request gpio(%d) fail", __func__, gpio_number);
	} else {
		HDMI_INFO("Set audio mute GPIO (%u)", gpio_value);
		gpio_direction_output(gpio_number, gpio_value);
		gpio_free(gpio_number);
	}
}

static void mute_gpio_timer_callback(unsigned long data)
{
	set_mute_gpio(audio_mute_gpio, AUDIO_MUTE_OFF);
}

void set_mute_gpio_pulse(void)
{
	if ((audio_mute_gpio < 0) || (mute_gpio_timer == NULL))
		return;

	set_mute_gpio(audio_mute_gpio, AUDIO_MUTE_ON);

	/* Disable mute after 3 sec */
	mod_timer(mute_gpio_timer, jiffies + msecs_to_jiffies(3000));
}

/**
 * setup_mute_gpio - Initial audio DAC mute pin control
 */
void setup_mute_gpio(struct device_node *dev_node)
{
	HDMI_INFO("[%s]", __func__);

	hdmitx_dev_node = dev_node;

	audio_mute_gpio = of_get_named_gpio(hdmitx_dev_node,
						"gpio-audio-mute", 0);

	if (audio_mute_gpio < 0) {
		HDMI_INFO("Undefined gpio-audio-mute, sikp %s", __func__);
		goto end;
	} else {
		HDMI_INFO("audio mute gpio(%d)", audio_mute_gpio);
	}

	set_mute_gpio(audio_mute_gpio, AUDIO_MUTE_OFF);

	mute_gpio_timer = kmalloc(sizeof(struct timer_list), GFP_KERNEL);
	if (mute_gpio_timer != NULL)
		setup_timer(mute_gpio_timer, mute_gpio_timer_callback, 0);
	else
		HDMI_ERROR("[%s] kmalloc fail", __func__);

end:
	return;
}
