/*
    acipcd.c Created on: Aug 3, 2010, Jinhua Huang <jhhuang@marvell.com>

    Marvell PXA9XX ACIPC-MSOCKET driver for Linux
    Copyright (C) 2010 Marvell International Ltd.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License version 2 as
    published by the Free Software Foundation.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <linux/module.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/device.h>
#include <linux/pm_wakeup.h>
#ifdef CONFIG_DDR_DEVFREQ
#include <linux/devfreq.h>
#endif
#include <linux/pm_qos.h>

#include "acipcd.h"
#include "portqueue.h"
#include "msocket.h"
#include "pxa_cp_load.h"

struct wakeup_source acipc_wakeup; /* used to ensure Workqueue scheduled. */
/* forward static function prototype, these all are interrupt call-backs */
#ifdef CONFIG_DDR_DEVFREQ
static u32 acipc_cb_modem_ddrfreq_update(u32 status);
#endif

static u32 acipc_cb_event_notify(u32 status);
static u32 acipc_cb_block_cpuidle_axi(u32 status);
#define RESET_CP_REQUEST 0x544F4F42
#define RESET_CP_REQUEST_DONE 0x454E4F44

#define REQ_AP_D2_STATUS  0x544F4F41  /* can enter D2 status */
#define REQ_AP_ND2_STATUS 0x533F3F31  /*  can't enter D2 status */
DECLARE_COMPLETION(reset_cp_confirm);

static u32 pm_qos_cpuidle_block_axi	= PM_QOS_CPUIDLE_BLOCK_DEFAULT_VALUE;
static struct pm_qos_request cp_block_cpuidle_axi = {
	.name = "cp_block_cpuidle_axi",
};

#ifdef CONFIG_DDR_DEVFREQ
static struct pm_qos_request modem_ddr_cons = {
	.name = "cp",
};
struct workqueue_struct *acipc_wq;
struct work_struct acipc_modem_ddr_freq_update;

#define MODEM_DDRFREQ_HI	400
#define MODEM_DDRFREQ_MID	312
#define MODEM_DDRFREQ_LOW	156
#define MHZ_TO_KHZ		1000
static void acipc_modem_ddr_freq_update_handler(struct work_struct *work)
{
	static int cur_ddrfreq;

	pr_info("acipc_cb_modem_ddrfreq_update: %d\n",
	       (unsigned int)cpks->modem_ddrfreq);
	if ((unsigned int)cpks->modem_ddrfreq ==
	    MODEM_DDRFREQ_HI) {
		if (cur_ddrfreq == MODEM_DDRFREQ_HI)
			return;
		pr_info("DDRFreq set to High\n");
		pm_qos_update_request(&modem_ddr_cons,
				      MODEM_DDRFREQ_HI * MHZ_TO_KHZ);
		cur_ddrfreq = MODEM_DDRFREQ_HI;
	}

	if ((unsigned int)cpks->modem_ddrfreq ==
	    MODEM_DDRFREQ_MID) {
		if (cur_ddrfreq == MODEM_DDRFREQ_MID)
			return;
		pr_info("DDRFreq set to Mid\n");
		pm_qos_update_request(&modem_ddr_cons,
				      MODEM_DDRFREQ_MID * MHZ_TO_KHZ);
		cur_ddrfreq = MODEM_DDRFREQ_MID;
	}

	if ((unsigned int)cpks->modem_ddrfreq ==
	    MODEM_DDRFREQ_LOW) {
		if (cur_ddrfreq == MODEM_DDRFREQ_LOW)
			return;
		pr_info("DDRFreq set to Low\n");
		pm_qos_update_request(&modem_ddr_cons, PM_QOS_DEFAULT_VALUE);
		cur_ddrfreq = MODEM_DDRFREQ_LOW;
	}
	return;
}
#endif

/* acipc_init is used to register interrupt call-back function */
int acipc_init(u32 lpm_qos)
{
	wakeup_source_init(&acipc_wakeup, "acipc_wakeup");

	/* we do not check any return value */
	acipc_event_bind(ACIPC_MODEM_DDR_UPDATE_REQ, acipc_cb_event_notify,
		       ACIPC_CB_NORMAL, NULL);

	acipc_event_bind(ACIPC_IPM, acipc_cb_block_cpuidle_axi,
		       ACIPC_CB_NORMAL, NULL);

	pm_qos_cpuidle_block_axi = lpm_qos;
	pm_qos_add_request(&cp_block_cpuidle_axi, PM_QOS_CPUIDLE_BLOCK,
		PM_QOS_CPUIDLE_BLOCK_DEFAULT_VALUE);

#ifdef CONFIG_DDR_DEVFREQ
	pm_qos_add_request(&modem_ddr_cons, PM_QOS_DDR_DEVFREQ_MIN,
		PM_QOS_DEFAULT_VALUE);
	INIT_WORK(&acipc_modem_ddr_freq_update,
		acipc_modem_ddr_freq_update_handler);
	acipc_wq = alloc_workqueue("ACIPC_WQ", WQ_HIGHPRI, 0);
#endif

	return 0;
}

/* acipc_exit used to unregister interrupt call-back function */
void acipc_exit(void)
{
	acipc_event_unbind(ACIPC_PORT_FLOWCONTROL);
	acipc_event_unbind(ACIPC_RINGBUF_TX_RESUME);
	acipc_event_unbind(ACIPC_RINGBUF_TX_STOP);
	acipc_event_unbind(ACIPC_MUDP_KEY);
	acipc_event_unbind(ACIPC_MODEM_DDR_UPDATE_REQ);

	acipc_event_unbind(ACIPC_IPM);

	pm_qos_remove_request(&cp_block_cpuidle_axi);

	wakeup_source_trash(&acipc_wakeup);

#ifdef CONFIG_DDR_DEVFREQ
	destroy_workqueue(acipc_wq);
	pm_qos_remove_request(&modem_ddr_cons);
#endif
}

#ifdef CONFIG_DDR_DEVFREQ
static u32 acipc_cb_modem_ddrfreq_update(u32 status)
{
	queue_work(acipc_wq, &acipc_modem_ddr_freq_update);

	return 0;
}
#endif

static u32 acipc_cb_reset_cp_confirm(u32 status)
{
	if (cpks->reset_request
		== RESET_CP_REQUEST_DONE)
		complete(&reset_cp_confirm);

	cpks->reset_request = 0;
	return 0;
}

static u32 acipc_cb_event_notify(u32 status)
{
	acipc_cb_reset_cp_confirm(status);
#ifdef CONFIG_DDR_DEVFREQ
	acipc_cb_modem_ddrfreq_update(status);
#endif
	return 0;
}

void acipc_reset_cp_request(void)
{
	mutex_lock(&cpks_lock);
	if (!cpks) {
		mutex_unlock(&cpks_lock);
		return;
	}
	cpks->reset_request = RESET_CP_REQUEST;
	reinit_completion(&reset_cp_confirm);
	acipc_notify_reset_cp_request();
	mutex_unlock(&cpks_lock);
	if (wait_for_completion_timeout(&reset_cp_confirm, 2 * HZ))
		pr_info("reset cp request success!\n");
	else
		pr_err("reset cp request fail!\n");

	mutex_lock(&cpks_lock);
	if (!cpks) {
		mutex_unlock(&cpks_lock);
		return;
	}
	cpks->reset_request = 0;
	mutex_unlock(&cpks_lock);
	return;
}

void acipc_ap_block_cpuidle_axi(bool block)
{
	pm_qos_update_request(&cp_block_cpuidle_axi,
		(block ? pm_qos_cpuidle_block_axi
			   : PM_QOS_CPUIDLE_BLOCK_DEFAULT_VALUE));
}
EXPORT_SYMBOL(acipc_ap_block_cpuidle_axi);

static u32 acipc_cb_block_cpuidle_axi(u32 status)
{
	bool block = false;
	u32 pm_status = 0;

	pm_status = cpks->ap_pm_status_request;
	if (pm_status == REQ_AP_D2_STATUS) {
		block = false;
	} else if (pm_status == REQ_AP_ND2_STATUS) {
		block = true;
	} else {
		pr_info("acipc: unknow status %d!!!\n", pm_status);
		return 0;
	}

	acipc_ap_block_cpuidle_axi(block);
	return 0;
}
