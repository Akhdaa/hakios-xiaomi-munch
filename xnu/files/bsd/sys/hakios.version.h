/*
 * The version of this kernel (drivers/core/hakios.version.c): sysctl kern.hakios.version and a line of the boot log.
 */
#ifndef _SYS_HAKIOS_VERSION_H_
#define _SYS_HAKIOS_VERSION_H_

#ifdef KERNEL
#include <stdbool.h>
/* Read the boot-arg hakios_kver, keep it as kern.hakios.version and say it in the log. Called from bsd_init(). */
void hakios_version_init(void);
/*
 * Whether the kernel was configured for this SoC family ("sc7280", "sm8250"): the CONFIG_SOC in hakios_kver, as
 * MiniIOMobileFramebuffer reads it. A driver that knows a chip's addresses asks this before it reads any of them.
 */
bool hakios_soc_is(const char *soc);
#endif /* KERNEL */

#endif /* _SYS_HAKIOS_VERSION_H_ */
