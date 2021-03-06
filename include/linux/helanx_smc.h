#ifndef __HELANX_SMC_H
#define __HELANX_SMC_H

#include <linux/notifier.h>

#define GPIO_0_ADDR		0xd401e0dc
#define GPIO_60_ADDR		0xd401e004
#define GPIO_67_ADDR		0xd401e1b8
#define GPIO_99_ADDR		0xd401e0b0
#define GPIO_100_ADDR		0xd401e238
#define GPIO_110_ADDR		0xd401e298
#define GPIO_117_ADDR		0xd401e0b4
#define GPIO_121_ADDR		0xd401e32c
#define GPIO_122_ADDR		0xd401e0c8
#define GPIO_126_ADDR		0xd401e06c

/* smc function id must be aligned with ATF */
#define LC_ADD_GPIO_EDGE_WAKEUP		0xc2003002
#define LC_REMOVE_GPIO_EDGE_WAKEUP	0xc2003003
#define LC_ADD_SHARE_ADDRESS		0xc2003004

extern noinline int __invoke_fn_smc(u64 function_id, u64 arg0, u64 arg1, u64 arg2);
extern int mfp_edge_wakeup_notify(struct notifier_block *nb,
				  unsigned long val, void *data);
extern int store_share_address(unsigned long addr, unsigned long len);

#endif

