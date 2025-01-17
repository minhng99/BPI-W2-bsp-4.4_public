#ifndef __ASM_IDLE_H
#define __ASM_IDLE_H

#include <linux/cpuidle.h>
#include <linux/linkage.h>

extern void (*cpu_wait)(void);
extern void r4k_wait(void);
extern asmlinkage void __r4k_wait(void);
extern void r4k_wait_irqoff(void);

static inline int using_rollback_handler(void)
{
#ifdef CONFIG_CPU_RLX
        return 0;
#else
	return cpu_wait == r4k_wait;
#endif
}

extern int mips_cpuidle_wait_enter(struct cpuidle_device *dev,
				   struct cpuidle_driver *drv, int index);

#define MIPS_CPUIDLE_WAIT_STATE {\
	.enter			= mips_cpuidle_wait_enter,\
	.exit_latency		= 1,\
	.target_residency	= 1,\
	.power_usage		= UINT_MAX,\
	.name			= "wait",\
	.desc			= "MIPS wait",\
}

#endif /* __ASM_IDLE_H  */
