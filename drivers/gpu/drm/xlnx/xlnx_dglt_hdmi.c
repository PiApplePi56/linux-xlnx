// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2023 Alientek Co.Ltd
 * Authors:
 * CX <2568365021m@qq.com>
 *
 * Based on drivers/gpu/drm/exynos/exynos_hdmi.c
 */

#define DEBUG

#include <linux/clk.h>
#include <linux/device.h>
#include <linux/component.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/hdmi.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/kernel.h>
#include <linux/of_device.h>
#include <linux/of_graph.h>
#include <linux/platform_device.h>
#include <linux/wait.h>

#include <drm/drmP.h>
#include <drm/drm_of.h>
#include <drm/drm_crtc_helper.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_connector.h>
#include <drm/drm_edid.h>
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>

#define HOTPLUG_DEBOUNCE_MS		1100
#define DIGILENT_ENC_MAX_FREQ 150000  //KHz
#define DIGILENT_ENC_MAX_H 1920
#define DIGILENT_ENC_MAX_V 1080
#define DIGILENT_ENC_PREF_H 1280
#define DIGILENT_ENC_PREF_V 720
#define DIGILENT_ENC_REFRESH_RATE 60

struct dglnt_hdmi {
	struct device			*dev;
	struct drm_device		*drm_dev;
	struct drm_encoder		encoder;
	struct drm_connector	connector;
	struct delayed_work		hotplug_work;

	struct i2c_adapter 		*ddc;
	struct gpio_desc		*hpd_gpio;
	struct clk_bulk_data 	*clks;
	int 					num_clks;
	int						irq;

	bool					dvi_mode;
	bool					enabled;
};

/* List of clocks required by HDMI tx subsystem */
static const char * const hdmi_clks[] = {
	"hdmi_pclk", "hdmi_pclk_5x", "hdmi_pclk_2_5x",
};

static inline struct dglnt_hdmi *encoder_to_hdmi(struct drm_encoder *e)
{
	return container_of(e, struct dglnt_hdmi, encoder);
}

static inline struct dglnt_hdmi *connector_to_hdmi(struct drm_connector *c)
{
	return container_of(c, struct dglnt_hdmi, connector);
}

static enum drm_connector_status hdmi_detect(struct drm_connector *connector,
				bool force)
{
	struct dglnt_hdmi *hdmi = connector_to_hdmi(connector);

	if (gpiod_get_value(hdmi->hpd_gpio))
		return connector_status_connected;

	return connector_status_disconnected;
}

static int hdmi_get_modes(struct drm_connector *connector)
{
	struct dglnt_hdmi *hdmi = connector_to_hdmi(connector);
	struct edid *edid;
	int ret = 0;

	if (!hdmi->ddc)
		return 0;

	edid = drm_get_edid(connector, hdmi->ddc);
	if (edid) {
		hdmi->dvi_mode = !drm_detect_monitor_audio(edid);
		DRM_DEV_DEBUG_KMS(hdmi->dev, "%s : width[%d] x height[%d]\n",
				  (hdmi->dvi_mode ? "dvi monitor" : "hdmi monitor"),
				  edid->width_cm, edid->height_cm);

		drm_connector_update_edid_property(connector, edid);

		ret = drm_add_edid_modes(connector, edid);

		kfree(edid);
	} else {
		DRM_DEV_DEBUG_KMS(hdmi->dev, "failed to get edid, use default.\n");
		/* In case we cannot retrieve the EDIDs (broken or missing i2c
		 * bus)*/
		ret= drm_add_modes_noedid(connector, 1920, 1200);
		drm_set_preferred_mode(connector, 1920, 1080); 
	}

	return ret;
}

static enum drm_mode_status hdmi_mode_valid(struct drm_connector *connector,
			struct drm_display_mode *mode)
{
	struct dglnt_hdmi *hdmi = connector_to_hdmi(connector);

	DRM_DEV_DEBUG_KMS(hdmi->dev,
			  "xres=%d, yres=%d, refresh=%d, intl=%d clock=%d\n",
			  mode->hdisplay, mode->vdisplay, drm_mode_vrefresh(mode),
			  (mode->flags & DRM_MODE_FLAG_INTERLACE) ? true :
			  false, mode->clock * 1000);

	if(!(mode->type & DRM_MODE_TYPE_DRIVER))
		return MODE_BAD;

	if (mode &&
       !(mode->flags & ((DRM_MODE_FLAG_INTERLACE | DRM_MODE_FLAG_DBLCLK) | DRM_MODE_FLAG_3D_MASK)) &&
       (mode->clock <= DIGILENT_ENC_MAX_FREQ) &&
       (mode->hdisplay <= DIGILENT_ENC_MAX_H) &&
       (mode->vdisplay <= DIGILENT_ENC_MAX_V) &&
	   (mode->vrefresh <= DIGILENT_ENC_REFRESH_RATE)){
        return MODE_OK;
    }

	return MODE_BAD;
}

static int
hdmi_probe_single_connector_modes(struct drm_connector *connector,
				       uint32_t maxX, uint32_t maxY)
{
	return drm_helper_probe_single_connector_modes(connector, 1920, 1080);
}

static void hdmi_connector_destroy(struct drm_connector *connector)
{
	drm_connector_unregister(connector);
	drm_connector_cleanup(connector);
}

static const struct drm_connector_funcs hdmi_connector_funcs = {
	.fill_modes = hdmi_probe_single_connector_modes,
	.detect = hdmi_detect,
	.destroy = hdmi_connector_destroy,
	.reset = drm_atomic_helper_connector_reset,
	.atomic_duplicate_state = drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state = drm_atomic_helper_connector_destroy_state,
};

static const struct drm_connector_helper_funcs hdmi_connector_helper_funcs = {
	.get_modes = hdmi_get_modes,
	.mode_valid = hdmi_mode_valid,
};

static void hdmi_encoder_mode_set(struct drm_encoder *encoder,
				       struct drm_display_mode *mode,
				       struct drm_display_mode *adj_mode)
{
	struct dglnt_hdmi *hdmi = encoder_to_hdmi(encoder);

		printk(KERN_WARNING "mode is %s\n",mode->name);
		printk(KERN_WARNING "type is %d\n",mode->type);
		printk(KERN_WARNING "crtc_clock is %d\n",mode->crtc_clock);
		printk(KERN_WARNING "vtotal is %d\n",mode->vtotal);
		printk(KERN_WARNING "htotal is %d\n",mode->crtc_htotal);

	//动态设置分辨率，自动根据分辨率设置相应的时钟
	//TODO: MPSoC 开发板——由于设置时钟后会导致lcd无法工作，原因是设置时钟频率会导致锁相环复位，从而导致LCD相关的模块复位，导致LCD出问题，所以暂不支持动态设置分辨率，默认只支持1080P
		clk_set_rate(hdmi->clks[0].clk, mode->crtc_clock * 1000);
		clk_set_rate(hdmi->clks[1].clk, mode->crtc_clock * 1000 * 5);

		if(3 == hdmi->num_clks)
			clk_set_rate(hdmi->clks[2].clk, mode->crtc_clock * 1000 * 5 >> 1);

}

static void hdmi_enable(struct drm_encoder *encoder)
{
	struct dglnt_hdmi *hdmi = encoder_to_hdmi(encoder);

	DRM_DEBUG_DRIVER("Enabling the HDMI Output\n");

	//动态设置分辨率，自动根据分辨率设置相应的时钟
	//TODO: MPSoC 开发板——由于设置时钟后会导致lcd无法工作，原因是设置时钟频率会导致锁相环复位，从而导致LCD相关的模块复位，导致LCD出问题，所以暂不支持动态设置分辨率，默认只支持1080P
	if(clk_bulk_prepare_enable(hdmi->num_clks, hdmi->clks)){
		DRM_DEV_ERROR(hdmi->dev,
			      "Cannot enable HDMI pclk clock!\n");
	}
	hdmi->enabled = true;
}

static void hdmi_disable(struct drm_encoder *encoder)
{
	struct dglnt_hdmi *hdmi = encoder_to_hdmi(encoder);

	DRM_DEBUG_DRIVER("Disabling the HDMI Output\n");

	hdmi->enabled = false;
	cancel_delayed_work(&hdmi->hotplug_work);
}

static const struct drm_encoder_helper_funcs atk_hdmi_encoder_helper_funcs = {
	.mode_set   = hdmi_encoder_mode_set,
	.enable		= hdmi_enable,
	.disable	= hdmi_disable,
};

static const struct drm_encoder_funcs atk_hdmi_encoder_funcs = {
	.destroy = drm_encoder_cleanup,
};

static void hdmi_hotplug_work_func(struct work_struct *work)
{
	struct dglnt_hdmi *hdmi;

	hdmi = container_of(work, struct dglnt_hdmi, hotplug_work.work);

	if (hdmi->drm_dev)
		drm_helper_hpd_irq_event(hdmi->drm_dev);
}

static irqreturn_t hdmi_irq_thread(int irq, void *arg)
{
	struct dglnt_hdmi *hdmi = arg;

	mod_delayed_work(system_wq, &hdmi->hotplug_work,
			msecs_to_jiffies(HOTPLUG_DEBOUNCE_MS));

	return IRQ_HANDLED;
}

static int hdmi_bind(struct device *dev, struct device *master, void *data)
{
	struct dglnt_hdmi *hdmi = dev_get_drvdata(dev);
	struct drm_encoder *encoder = &hdmi->encoder;
	struct drm_device *drm_dev = data;
	struct device_node *port;
	int ret;

	hdmi->drm_dev = drm_dev;

	encoder->possible_crtcs = 1;

	for_each_child_of_node(dev->of_node, port) {
		if (!port->name || of_node_cmp(port->name, "ports")) {
			DRM_INFO("port name is null or node name is not ports!\n");
			continue;
		}
		encoder->possible_crtcs |= drm_of_find_possible_crtcs(drm_dev, port);
	}

	/* initialize encoder */
	drm_encoder_init(drm_dev, encoder, &atk_hdmi_encoder_funcs,
		 DRM_MODE_ENCODER_TMDS, NULL);
	drm_encoder_helper_add(encoder, &atk_hdmi_encoder_helper_funcs);

	hdmi->connector.interlace_allowed = true;
	hdmi->connector.polled = DRM_CONNECTOR_POLL_HPD;

	/* initialize connector */
	ret = drm_connector_init(drm_dev, &hdmi->connector,
			&hdmi_connector_funcs, DRM_MODE_CONNECTOR_HDMIA);
	if (ret) {
		DRM_DEV_ERROR(hdmi->dev,
			      "Failed to initialize connector with drm\n");
		drm_encoder_cleanup(encoder);
		return ret;
	}
	drm_connector_helper_add(&hdmi->connector, &hdmi_connector_helper_funcs);
	drm_connector_register(&hdmi->connector);
	drm_connector_attach_encoder(&hdmi->connector, encoder);

	return ret;
}

static void hdmi_unbind(struct device *dev, struct device *master, void *data)
{
	struct dglnt_hdmi *hdmi = dev_get_drvdata(dev);

	hdmi->connector.funcs->destroy(&hdmi->connector);
	hdmi->encoder.funcs->destroy(&hdmi->encoder);

	i2c_put_adapter(hdmi->ddc);
	clk_bulk_disable_unprepare(hdmi->num_clks,hdmi->clks);

	cancel_delayed_work_sync(&hdmi->hotplug_work);
}

static const struct component_ops hdmi_component_ops = {
	.bind	= hdmi_bind,
	.unbind = hdmi_unbind,
};

static int hdmi_resources_init(struct dglnt_hdmi *hdmi)
{
	struct device *dev = hdmi->dev;
	struct device_node *ddc;
	int ret;
	u_char i;

	DRM_DEV_DEBUG_KMS(dev, "HDMI resource init\n");

	hdmi->hpd_gpio = devm_gpiod_get(dev, "hpd", GPIOD_IN);
	if (IS_ERR(hdmi->hpd_gpio)) {
		DRM_DEV_ERROR(dev, "cannot get hpd gpio property\n");
		return PTR_ERR(hdmi->hpd_gpio);
	}

	hdmi->irq = gpiod_to_irq(hdmi->hpd_gpio);
	if (hdmi->irq < 0) {
		DRM_DEV_ERROR(dev, "failed to get GPIO irq\n");
		return  hdmi->irq;
	}

	ddc = of_parse_phandle(dev->of_node, "ddc-i2c-bus", 0);
	if (ddc) {
		hdmi->ddc = of_find_i2c_adapter_by_node(ddc);
		of_node_put(ddc);

		if (hdmi->ddc == NULL) {
			DRM_INFO("failed get ddc i2c adapter by node\n");
			return -EPROBE_DEFER;
		}
	} else {
		DRM_DEV_ERROR(hdmi->dev, "no ddc-i2c-bus property found\n");
		return -EPROBE_DEFER;
	}

	DRM_INFO("Success to get ddc i2c adapter by node\n");

	ret = of_property_read_u32(dev->of_node, "clk-num", &hdmi->num_clks);
	if (ret || hdmi->num_clks > ARRAY_SIZE(hdmi_clks)) {
		DRM_DEV_ERROR(hdmi->dev, "no clk-num property found\n");
		return ret;
	}
	hdmi->clks = devm_kcalloc(dev, hdmi->num_clks,
				  sizeof(*hdmi->clks), GFP_KERNEL);
	if (!hdmi->clks)
		return -ENOMEM;

	for (i = 0; i < hdmi->num_clks; i++)
		hdmi->clks[i].id = hdmi_clks[i];

	ret = devm_clk_bulk_get(dev, hdmi->num_clks, hdmi->clks);
	if (ret){
		DRM_DEV_ERROR(dev, "failed to get hdmi pclks\n");
		return ret;
	}

	ret = clk_bulk_prepare_enable(hdmi->num_clks, hdmi->clks);
	if (ret) {
		DRM_DEV_ERROR(hdmi->dev,
			      "Cannot enable HDMI pclk clock: %d\n", ret);
	}

	return ret;
}

static int hdmi_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct dglnt_hdmi *hdmi;
	int ret;

	hdmi = devm_kzalloc(dev, sizeof(struct dglnt_hdmi), GFP_KERNEL);
	if (!hdmi)
		return -ENOMEM;

	hdmi->dev = dev;

	ret = hdmi_resources_init(hdmi);
	if (ret) {
		DRM_DEBUG_DRIVER("hdmi_resources_init failed\n");
		return ret;
	}

	platform_set_drvdata(pdev, hdmi);

	INIT_DELAYED_WORK(&hdmi->hotplug_work, hdmi_hotplug_work_func);

	ret = devm_request_threaded_irq(dev, hdmi->irq, NULL,
			hdmi_irq_thread, IRQF_TRIGGER_RISING |
			IRQF_TRIGGER_FALLING | IRQF_ONESHOT,
			"hdmi", hdmi);
	if (ret) {
		DRM_DEV_ERROR(dev, "failed to register hdmi interrupt\n");
		goto err_ddc;
	}

	ret = component_add(&pdev->dev, &hdmi_component_ops);
	if (ret)
		goto err_ddc;

	return ret;

err_ddc:
	i2c_put_adapter(hdmi->ddc);
	clk_bulk_disable_unprepare(hdmi->num_clks,hdmi->clks);

	return ret;
}

static int hdmi_remove(struct platform_device *pdev)
{
	component_del(&pdev->dev, &hdmi_component_ops);

	return 0;
}

static const struct of_device_id hdmi_of_match[] = {
	{ .compatible = "digilent,dglnt-hdmi" },
	{ }
};
MODULE_DEVICE_TABLE(of, hdmi_of_match);


static struct platform_driver hdmi_driver = {
	.probe		= hdmi_probe,
	.remove		= hdmi_remove,
	.driver		= {
		.name	= "dglnt-hdmi",
		.owner	= THIS_MODULE,
		.of_match_table = hdmi_of_match,
	},
};
module_platform_driver(hdmi_driver);

MODULE_AUTHOR("CX <2568365021@qq.com>");
MODULE_DESCRIPTION("Digilent FPGA HDMI Tx Driver");
MODULE_LICENSE("GPL v2");
