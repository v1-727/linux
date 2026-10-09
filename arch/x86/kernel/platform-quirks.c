// SPDX-License-Identifier: GPL-2.0
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/pnp.h>

#include <asm/setup.h>
#include <asm/bios_ebda.h>
#include <asm/cpu.h>
#include <asm/cpu_device_id.h>
#include <asm/cpuid/api.h>
#include <asm/intel-family.h>

// Detect Intel MID with no dependence on hardware_subarch
static bool __init x86_cpu_is_intel_mid(void)
{
	u32 eax = 0, ebx, ecx = 0, edx;
	unsigned int sig, vfm;

	if (!IS_ENABLED(CONFIG_X86_INTEL_MID) || !cpuid_feature())
		return false;

	native_cpuid(&eax, &ebx, &ecx, &edx);
	/* "GenuineIntel" */
	if (ebx != 0x756e6547 || edx != 0x49656e69 || ecx != 0x6c65746e)
		return false;

	sig = native_cpuid_eax(1);
	vfm = IFM(x86_family(sig), x86_model(sig));

	return vfm == INTEL_ATOM_SILVERMONT_MID ||
	       vfm == INTEL_ATOM_SILVERMONT_MID2;
}

void __init x86_early_init_platform_quirks(void)
{
	if (boot_params.hdr.hardware_subarch == X86_SUBARCH_PC &&
	    x86_cpu_is_intel_mid())
		boot_params.hdr.hardware_subarch = X86_SUBARCH_INTEL_MID;

	x86_platform.legacy.i8042 = X86_LEGACY_I8042_EXPECTED_PRESENT;
	x86_platform.legacy.rtc = 1;
	x86_platform.legacy.warm_reset = 1;
	x86_platform.legacy.reserve_bios_regions = 0;
	x86_platform.legacy.devices.pnpbios = 1;

	switch (boot_params.hdr.hardware_subarch) {
	case X86_SUBARCH_PC:
		x86_platform.legacy.reserve_bios_regions = 1;
		break;
	case X86_SUBARCH_XEN:
		x86_platform.legacy.devices.pnpbios = 0;
		x86_platform.legacy.rtc = 0;
		break;
	case X86_SUBARCH_INTEL_MID:
	case X86_SUBARCH_CE4100:
		x86_platform.legacy.devices.pnpbios = 0;
		x86_platform.legacy.rtc = 0;
		x86_platform.legacy.i8042 = X86_LEGACY_I8042_PLATFORM_ABSENT;
		break;
	}

	if (x86_platform.set_legacy_features)
		x86_platform.set_legacy_features();
}

bool __init x86_pnpbios_disabled(void)
{
	return x86_platform.legacy.devices.pnpbios == 0;
}

#if defined(CONFIG_PNPBIOS)
bool __init arch_pnpbios_disabled(void)
{
	return x86_pnpbios_disabled();
}
#endif
