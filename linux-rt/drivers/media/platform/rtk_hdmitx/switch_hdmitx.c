#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/switch.h>
#include <linux/device.h>
#include <linux/kthread.h>
#include <linux/delay.h>

#include <linux/platform_device.h>
#include <linux/gpio.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/workqueue.h>
#include <linux/slab.h>

#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_irq.h>
#include <linux/of_gpio.h>

#include "hdmitx.h"
#include "hdmitx_dev.h"
#include "hdmitx_api.h"
#include "rtk_edid.h"
#include "hdmitx_scdc.h"

#define HDMI_SWITCH_NAME "hdmi"

extern struct edid_info hdmitx_edid_info;

#ifdef CONFIG_RTK_HDMIRX
extern void HdmiRx_save_tx_physical_addr(unsigned char byteMSB, unsigned char byteLSB);
#ifdef CONFIG_RTK_HDCPRX_2P2
extern void Hdmi_SetHPD(char high);
#else
#define Hdmi_SetHPD(format, ...)
#endif /* CONFIG_RTK_HDCPRX_2P2 */
#else
#define HdmiRx_save_tx_physical_addr(format, ...)
#define Hdmi_SetHPD(format, ...)
#endif /* CONFIG_RTK_HDMIRX */

static struct hdmitx_switch_data s_data;
static struct switch_dev *sdev = NULL;

#if HDMI_RX_SENSE_SUPPORT
static struct task_struct *hdmitx_hpd_tsk;
#define RX_SENSE_COUNT_MAX 2
#endif

static ssize_t hdmitx_switch_print_state(struct switch_dev *sdev, char *buffer)
{
	HDMI_DEBUG("hdmitx_switch_print_state");
	return sprintf(buffer, "%d", s_data.state);
}

int hdmitx_switch_get_state(void)
{
	return s_data.state;
}

#if HDMI_RX_SENSE_SUPPORT
int hdmitx_switch_get_hpd(void)
{
	return s_data.hpd_state;
}

void hdmitx_send_hdmioff(void)
{
	struct VIDEO_RPC_VOUT_CONFIG_TV_SYSTEM tv_system;

	HDMI_INFO("[%s]",__FUNCTION__);

	memset(&tv_system, 0, sizeof(tv_system));
	tv_system.videoInfo.standard = VO_STANDARD_NTSC_J;
	tv_system.videoInfo.enProg = 0x1;
	tv_system.videoInfo.pedType = 0x1;
	tv_system.videoInfo.dataInt0 = 0x4;
	tv_system.hdmiInfo.hdmiMode = VO_HDMI_OFF;
	tv_system.hdmiInfo.hdmi_off_mode = VO_HDMI_OFF_CLOCK_OFF;

	RPC_TOAGENT_HDMI_Config_TV_System(&tv_system);
}

static int hdmitx_switch_thread(void *arg)
{
	int state=0,hpd;
	unsigned char rxsense,rxsense_count=0;
	hdmitx_device_t *pdev = container_of(sdev, hdmitx_device_t,sdev);

	for(;;)
	{
		if (kthread_should_stop()) break;

		hpd = gpio_get_value(s_data.pin);

		if(hpd)
		{
			if(s_data.hpd_state==1)
				rxsense = hdmitx_check_rx_sense(pdev->reg_base);
			else//Clk is off, don't read rx sense for prevent sb2 error
				rxsense = 1;

			if((rxsense==0)&&(rxsense_count<RX_SENSE_COUNT_MAX))
			{
				rxsense_count++;
				state = 1;
			}
			else if((rxsense==0)&&(rxsense_count>=RX_SENSE_COUNT_MAX))
			{
				state = 0;
			}
			else
				state = 1;
		}
		else
		{
			rxsense = 0;
			rxsense_count = 0;
			state = 0;

			//Pulled out cable when switch state is 0, send hdmi off rpc
			if((s_data.hpd_state==1)&&(s_data.state==0))
			{
				s_data.hpd_state = 0;
				hdmitx_send_hdmioff();
			}
		}

		s_data.hpd_state = hpd;

		if(state != s_data.state)
		{
			HDMI_INFO("%s:start, HPD(%d) RX_Sense(%u)", state?"plugged in":"pulled out", hpd, rxsense);

			if(state == 1)
			{
				hdmitx_get_sink_capability((asoc_hdmi_t*)hdmitx_get_drvdata(pdev));
				rxsense_count = 0;
			}
			else
				hdmitx_reset_sink_capability((asoc_hdmi_t*)hdmitx_get_drvdata(pdev));

			s_data.state = state;
			switch_set_state(sdev, s_data.state);
			HDMI_INFO("%s:done", s_data.state?"plugged in":"pulled out");
		}

		msleep(700);
	}

	return 0;
}
#else
static void hdmitx_switch_work_func(struct work_struct *work) 
{
	int state =0,sink_changed=0;		
	hdmitx_device_t *pdev = container_of(sdev, hdmitx_device_t,sdev);
	asoc_hdmi_t *drvdata = (asoc_hdmi_t*)hdmitx_get_drvdata(pdev);

	state = gpio_get_value(s_data.pin);	
	s_data.state = state;

	HDMI_INFO("%s:start", state?"plugged in":"pulled out");
	
	if(state == 1)
		sink_changed = hdmitx_get_sink_capability(drvdata);
	else
    {       
		hdmitx_turn_off_tmds(HDMI_MODE_HDMI);     
		hdmitx_reset_sink_capability(drvdata);
    } 

	if(sink_changed)	
	{		
		state = gpio_get_value(s_data.pin);
		s_data.state = state;
		
		hdmitx_dump_error_code();		
	}
	
	HDMI_INFO("%s:done", state?"plugged in":"pulled out");

	if(s_data.state!= switch_get_state(sdev))
	{
		switch_set_state(sdev, s_data.state);
		HDMI_INFO("Switch state to %u",s_data.state);

		if(s_data.state==1)
		{
			/* HDMI 1.4 CTS 9-5 PA increment, also include hotplug pluse for HDCP repeater CTS */
			HdmiRx_save_tx_physical_addr(drvdata->sink_cap.cec_phy_addr[0], drvdata->sink_cap.cec_phy_addr[1]);

			if(hdmitx_edid_info.scdc_capable&SCDC_RR_CAPABLE)
				enable_hdmitx_scdcrr(1);
		}
		else
		{
			Hdmi_SetHPD(0);  /* Set HDMI RX HPD */
			enable_hdmitx_scdcrr(0);
		}
	}

	wake_up_interruptible(&pdev->hpd_wait);
}
#endif

static irqreturn_t hdmitx_switch_isr(int irq, void *data)
{    		
	schedule_work(&s_data.work);	
	HDMI_DEBUG("hdmitx_switch_isr");
	
	return IRQ_HANDLED;
}

int register_hdmitx_switchdev(hdmitx_device_t * device)
{
	int ret;
			
	HDMI_DEBUG("register_hdmitx_switch");	

	if (&device->sdev==NULL)
		return -ENOMEM;
		
	sdev = &device->sdev;
	
	sdev->name = HDMI_SWITCH_NAME;
	sdev->print_state = hdmitx_switch_print_state;

	ret = switch_dev_register(sdev);
	if (ret < 0) {
		HDMI_ERROR("err_register_switch");
		goto err_register_switch;
	}

	// Get hotplug pin state
	s_data.pin  = device->hpd_gpio;
	gpio_direction_input(s_data.pin);
	gpio_set_debounce(s_data.pin,30*1000); //30ms
	s_data.state = gpio_get_value(s_data.pin);

#if HDMI_RX_SENSE_SUPPORT
	// Hotplug/Rxsense detect polling therad
	s_data.state = 0;
	hdmitx_hpd_tsk = kthread_run(hdmitx_switch_thread, sdev, "hdmitx_hpd_thread");
	if(IS_ERR(hdmitx_hpd_tsk))
	{
		HDMI_ERROR("Create hdmitx_hpd_tsk fail");
		goto err_register_switch;
	}
#else
	// Init hotplug work function and ISR
	INIT_WORK(&s_data.work, hdmitx_switch_work_func);

	if(s_data.state)
		schedule_work(&s_data.work);

	s_data.irq = device->hpd_irq;
		
	irq_set_irq_type(s_data.irq, IRQ_TYPE_EDGE_BOTH);
	if(request_irq(s_data.irq, hdmitx_switch_isr,IRQF_SHARED,"switch_hdmitx",&device->dev)) {
		HDMI_ERROR("cannot register IRQ %d", s_data.irq);
	}
#endif

	goto end;

err_register_switch:	
	HDMI_ERROR("register_hdmitx_switch failed");
end:	
	return ret;
}
EXPORT_SYMBOL(register_hdmitx_switchdev);


void deregister_hdmitx_switchdev(hdmitx_device_t* device)
{
    return switch_dev_unregister(&device-> sdev);
}
EXPORT_SYMBOL(deregister_hdmitx_switchdev);


int show_hpd_status(bool real_time)
{		
	if(real_time)
		return gpio_get_value(s_data.pin);	
	else
		return s_data.state;
}
EXPORT_SYMBOL(show_hpd_status);


int rtk_hdmitx_switch_suspend(void)
{	
	int state=0;	
	hdmitx_device_t *pdev = container_of(sdev, hdmitx_device_t,sdev);

#if HDMI_RX_SENSE_SUPPORT
	if(!kthread_stop(hdmitx_hpd_tsk))
		HDMI_INFO("hdmitx_hpd_tsk stopped");
	else
		HDMI_ERROR("Stop hdmitx_hpd_tsk fail");
#else
	free_irq(s_data.irq, &pdev->dev);
	HDMI_DEBUG("%s free irq=%x ",__FUNCTION__,s_data.irq);

	cancel_work_sync(&s_data.work);//Cancel work and wait for it to finish
#endif

	s_data.state = state;		
	hdmitx_reset_sink_capability((asoc_hdmi_t*)hdmitx_get_drvdata(pdev));	
	switch_set_state(sdev, state);	

	HDMI_INFO("Switch state to %u",s_data.state);
			
	return 0;
}
EXPORT_SYMBOL(rtk_hdmitx_switch_suspend);

int rtk_hdmitx_switch_resume(void)
{	
	hdmitx_device_t *pdev = container_of(sdev, hdmitx_device_t,sdev);

	gpio_set_debounce(s_data.pin,30*1000); //30ms

#if HDMI_RX_SENSE_SUPPORT
	hdmitx_hpd_tsk = kthread_run(hdmitx_switch_thread, sdev, "hdmitx_hpd_thread");
	if(IS_ERR(hdmitx_hpd_tsk))
		HDMI_ERROR("Create hdmitx_hpd_tsk fail");
	else
		HDMI_INFO("Wake up hdmitx_hpd_tsk");
#else
	irq_set_irq_type(s_data.irq, IRQ_TYPE_EDGE_BOTH);
	
	if(request_irq(s_data.irq, hdmitx_switch_isr,IRQF_SHARED,"switch_hdmitx",&pdev->dev)) {
		HDMI_ERROR("cannot register IRQ %d", s_data.irq);
	}

	schedule_work(&s_data.work);
#endif

	return 0;
}
EXPORT_SYMBOL(rtk_hdmitx_switch_resume);

