//
//  kern_smoother.cpp
//  AppleBacklightSmoother
//
//  Copyright © 2020 Le Bao Hiep. All rights reserved.
//

#include <Headers/plugin_start.hpp>
#include <Headers/kern_api.hpp>
#include <Headers/kern_devinfo.hpp>
#include <IOKit/IOCommandGate.h>

#include "kern_smoother.hpp"

static const char kextVersion[] {
#ifdef DEBUG
	'D', 'B', 'G', '-',
#else
	'R', 'E', 'L', '-',
#endif
	xStringify(MODULE_VERSION)[0], xStringify(MODULE_VERSION)[2], xStringify(MODULE_VERSION)[4], '-',
	getBuildYear<0>(), getBuildYear<1>(), getBuildYear<2>(), getBuildYear<3>(), '-',
	getBuildMonth<0>(), getBuildMonth<1>(), '-', getBuildDay<0>(), getBuildDay<1>(), '\0'
};

OSDefineMetaClassAndStructors(PRODUCT_NAME, IOService)

PRODUCT_NAME *ADDPR(selfInstance) = nullptr;

IOService *PRODUCT_NAME::probe(IOService *provider, SInt32 *score) {
	ADDPR(selfInstance) = this;
	setProperty("VersionInfo", kextVersion);
	auto service = IOService::probe(provider, score);
	return ADDPR(startSuccess) ? service : nullptr;
}

bool PRODUCT_NAME::start(IOService *provider) {
	ADDPR(selfInstance) = this;
	if (!IOService::start(provider)) {
		SYSLOG("start", "failed to start the parent");
		return false;
	}

	workLoop = ADDPR(selfInstance)->getWorkLoop();
	if (!workLoop) {
		SYSLOG("start", "failed to get workloop");
		return false;
	}

	cmdGate = IOCommandGate::commandGate(this);
	if (!cmdGate || workLoop->addEventSource(cmdGate) != kIOReturnSuccess) {
		SYSLOG("start", "failed to open command gate");
		return false;
	}

	return ADDPR(startSuccess);
}

void PRODUCT_NAME::stop(IOService *provider) {
	ADDPR(selfInstance) = nullptr;
	if (cmdGate) {
		workLoop->removeEventSource(cmdGate);
		OSSafeReleaseNULL(cmdGate);
	}
	if (workLoop) {
		OSSafeReleaseNULL(workLoop);
	}
	IOService::stop(provider);
}

void PRODUCT_NAME::init_plugin() {
	workLoop = nullptr;
	cmdGate = nullptr;
	currentFramebuffer = nullptr;
	currentFramebufferOpt = nullptr;
	orgReadRegister32 = nullptr;
	orgWriteRegister32 = nullptr;
	backlightValueAssigned = false;
	currentBacklightValue = 0;
	targetBacklightFrequency = 0;
	targetPwmControl = 0;
	driverBacklightFrequency = 0;

	auto &bdi = BaseDeviceInfo::get();
	auto generation = bdi.cpuGeneration;
	auto family = bdi.cpuFamily;
	auto model = bdi.cpuModel;
	switch (generation) {
		case CPUInfo::CpuGeneration::Penryn:
		case CPUInfo::CpuGeneration::Nehalem:
			// Do not warn about legacy processors
			break;
		case CPUInfo::CpuGeneration::Westmere:
			currentFramebuffer = &kextIntelHDFb;
			break;
		case CPUInfo::CpuGeneration::SandyBridge:
			currentFramebuffer = &kextIntelSNBFb;
			break;
		case CPUInfo::CpuGeneration::IvyBridge:
			currentFramebuffer = &kextIntelCapriFb;
			break;
		case CPUInfo::CpuGeneration::Haswell:
			currentFramebuffer = &kextIntelAzulFb;
			break;
		case CPUInfo::CpuGeneration::Broadwell:
			currentFramebuffer = &kextIntelBDWFb;
			break;
		case CPUInfo::CpuGeneration::Skylake:
			currentFramebuffer = &kextIntelSKLFb;
			break;
		case CPUInfo::CpuGeneration::KabyLake:
			currentFramebuffer = &kextIntelKBLFb;
			break;
		case CPUInfo::CpuGeneration::CoffeeLake:
			currentFramebuffer = &kextIntelCFLFb;
			currentFramebufferOpt = &kextIntelKBLFb;
			break;
		case CPUInfo::CpuGeneration::CannonLake:
			currentFramebuffer = &kextIntelCNLFb;
			break;
		case CPUInfo::CpuGeneration::IceLake:
			currentFramebuffer = &kextIntelICLLPFb;
			currentFramebufferOpt = &kextIntelICLHPFb;
			break;
		case CPUInfo::CpuGeneration::CometLake:
			currentFramebuffer = &kextIntelCFLFb;
			currentFramebufferOpt = &kextIntelKBLFb;
			break;
		default:
			SYSLOG("smoother", "found an unsupported processor 0x%X:0x%X, please report this!", family, model);
			break;
	}

	if (currentFramebuffer)
		lilu.onKextLoadForce(currentFramebuffer);

	if (currentFramebufferOpt)
		lilu.onKextLoadForce(currentFramebufferOpt);

	lilu.onKextLoadForce(nullptr, 0,
	[](void *user, KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
		PRODUCT_NAME::processKext(patcher, index, address, size);
	}, nullptr);
}

void PRODUCT_NAME::processKext(KernelPatcher &patcher, size_t index, mach_vm_address_t address, size_t size) {
	if ((currentFramebuffer && currentFramebuffer->loadIndex == index) ||
		(currentFramebufferOpt && currentFramebufferOpt->loadIndex == index)) {
		// Find actual framebuffer used
		auto realFramebuffer = (currentFramebuffer && currentFramebuffer->loadIndex == index) ? currentFramebuffer : currentFramebufferOpt;

		orgReadRegister32 = patcher.solveSymbol<decltype(orgReadRegister32)>(index, "__ZN31AppleIntelFramebufferController14ReadRegister32Em", address, size);
		if (!orgReadRegister32) {
			SYSLOG("smoother", "Failed to find ReadRegister32");
			patcher.clearError();
			return;
		}

		orgWriteRegister32 = patcher.solveSymbol<decltype(orgWriteRegister32)>(index, "__ZN31AppleIntelFramebufferController15WriteRegister32Emj", address, size);
		if (!orgReadRegister32) {
			SYSLOG("smoother", "Failed to find WriteRegister32");
			patcher.clearError();
			return;
		}

		patcher.eraseCoverageInstPrefix(reinterpret_cast<mach_vm_address_t>(orgWriteRegister32));
	
		auto cpuGeneration = BaseDeviceInfo::get().cpuGeneration;
		decltype(wrapIvyWriteRegister32) *wrapWriteRegister32;
		if (cpuGeneration <= CPUInfo::CpuGeneration::IvyBridge) {
			wrapWriteRegister32 = wrapIvyWriteRegister32;
		} else if (cpuGeneration <= CPUInfo::CpuGeneration::KabyLake) {
			wrapWriteRegister32 = wrapHswWriteRegister32;
		} else {
			if (realFramebuffer == &kextIntelCFLFb) {
				wrapWriteRegister32 = wrapCflRealWriteRegister32;
			} else {
				wrapWriteRegister32 = wrapCflFakeWriteRegister32;
			}
		}

		orgWriteRegister32 = reinterpret_cast<decltype(orgWriteRegister32)>(patcher.routeFunction(reinterpret_cast<mach_vm_address_t>(orgWriteRegister32), reinterpret_cast<mach_vm_address_t>(wrapWriteRegister32), true));

		if (!orgWriteRegister32) {
			SYSLOG("smoother", "Failed to route WriteRegister32");
			patcher.clearError();
			return;
		}

		DBGLOG("smoother", "Successfully routed hwSetBacklight");
	}
}

void PRODUCT_NAME::wrapIvyWriteRegister32(void *that, uint32_t reg, uint32_t value) {
	if (reg == BXT_BLC_PWM_FREQ1) {
		if (value && value != driverBacklightFrequency) {
			DBGLOG("smoother", "wrapIvyWriteRegister32: driver requested BXT_BLC_PWM_FREQ1 = 0x%x", value);
			driverBacklightFrequency = value;
		}

		if (targetBacklightFrequency == 0) {
			// Save the hardware PWM frequency as initially set up by the system firmware.
			// We'll need this to restore later after system sleep.
			targetBacklightFrequency = orgReadRegister32(that, BXT_BLC_PWM_FREQ1);
			DBGLOG("smoother", "wrapIvyWriteRegister32: system initialized BXT_BLC_PWM_FREQ1 = 0x%x", targetBacklightFrequency);

			if (targetBacklightFrequency == 0) {
				// This should not happen with correctly written bootloader code, but in case it does, let's use a failsafe default value.
				targetBacklightFrequency = FallbackTargetBacklightFrequency;
				SYSLOG("smoother", "wrapIvyWriteRegister32: system initialized BXT_BLC_PWM_FREQ1 is ZERO");
			}
		}

		if (value) {
			// Nonzero writes to this register need to use the original system value.
			// Yet the driver can safely write zero to this register as part of system sleep.
			value = targetBacklightFrequency;
		}
	} else if (reg == BLC_PWM_CPU_CTL) {
		if (driverBacklightFrequency && targetBacklightFrequency) {
			// Translate the PWM duty cycle between the driver scale value and the HW scale value
			uint32_t rescaledValue = static_cast<uint32_t>((static_cast<uint64_t>(value) * static_cast<uint64_t>(targetBacklightFrequency)) / static_cast<uint64_t>(driverBacklightFrequency));
			DBGLOG("smoother", "wrapIvyWriteRegister32: write BLC_PWM_CPU_CTL 0x%x/0x%x, rescaled to 0x%x/0x%x", value, driverBacklightFrequency, rescaledValue, targetBacklightFrequency);
			value = rescaledValue;
		} else {
			// This should never happen, but in case it does we should log it at the very least.
			SYSLOG("smoother", "wrapIvyWriteRegister32: write BLC_PWM_CPU_CTL has zero frequency driver (%d) target (%d)", driverBacklightFrequency, targetBacklightFrequency);
		}
	}

	orgWriteRegister32(that, reg, value);
}

void PRODUCT_NAME::wrapHswWriteRegister32(void *that, uint32_t reg, uint32_t value) {
	if (reg == BXT_BLC_PWM_FREQ1) {
		// BXT_BLC_PWM_FREQ1 controls the backlight intensity.
		// High 16 of this register are the denominator (frequency), low 16 are the numerator (duty cycle).

		if (targetBacklightFrequency == 0) {
			// Populate the hardware PWM frequency as initially set up by the system firmware.
			uint32_t org_value = orgReadRegister32(that, BXT_BLC_PWM_FREQ1);
			targetBacklightFrequency = (org_value & 0xffff0000U) >> 16U;
			DBGLOG("smoother", "wrapHswWriteRegister32: system initialized PWM frequency = 0x%x", targetBacklightFrequency);

			if (targetBacklightFrequency == 0) {
				// This should not happen with correctly written bootloader code, but in case it does, let's use a failsafe default value.
				targetBacklightFrequency = FallbackTargetBacklightFrequency;
				SYSLOG("smoother", "wrapHswWriteRegister32: system initialized PWM frequency is ZERO");
			}
		}

		uint32_t dutyCycle = value & 0xffffU;
		uint32_t frequency = (value & 0xffff0000U) >> 16U;

		uint32_t rescaledDutyCycle = frequency == 0 ? 0 : static_cast<uint32_t>((static_cast<uint64_t>(dutyCycle) * static_cast<uint64_t>(targetBacklightFrequency)) / static_cast<uint64_t>(frequency));

		if (frequency) {
			// Nonzero writes to frequency need to use the original system frequency.
			// Yet the driver can safely write zero to this register as part of system sleep.
			frequency = targetBacklightFrequency;
		}

		// Write the rescaled duty cycle and frequency
		// FIXME: what if rescaled duty cycle overflow unsigned 16 bit int?
		value = (frequency << 16U) | rescaledDutyCycle;
	}

	orgWriteRegister32(that, reg, value);
}

void PRODUCT_NAME::wrapCflRealWriteRegister32(void *that, uint32_t reg, uint32_t value) {
	if (reg == BXT_BLC_PWM_FREQ1) {
		if (value && value != driverBacklightFrequency) {
			DBGLOG("smoother", "wrapCflRealWriteRegister32: driver requested BXT_BLC_PWM_FREQ1 = 0x%x", value);
			driverBacklightFrequency = value;
		}

		if (targetBacklightFrequency == 0) {
			// Save the hardware PWM frequency as initially set up by the system firmware.
			// We'll need this to restore later after system sleep.
			targetBacklightFrequency = orgReadRegister32(that, BXT_BLC_PWM_FREQ1);
			DBGLOG("smoother", "wrapCflRealWriteRegister32: system initialized BXT_BLC_PWM_FREQ1 = 0x%x", targetBacklightFrequency);

			if (targetBacklightFrequency == 0) {
				// This should not happen with correctly written bootloader code, but in case it does, let's use a failsafe default value.
				targetBacklightFrequency = FallbackTargetBacklightFrequency;
				SYSLOG("smoother", "wrapCflRealWriteRegister32: system initialized BXT_BLC_PWM_FREQ1 is ZERO");
			}
		}

		if (value) {
			// Nonzero writes to this register need to use the original system value.
			// Yet the driver can safely write zero to this register as part of system sleep.
			value = targetBacklightFrequency;
		}
	} else if (reg == BXT_BLC_PWM_DUTY1) {
		if (driverBacklightFrequency && targetBacklightFrequency) {
			// Translate the PWM duty cycle between the driver scale value and the HW scale value
			uint32_t rescaledValue = static_cast<uint32_t>((static_cast<uint64_t>(value) * static_cast<uint64_t>(targetBacklightFrequency)) / static_cast<uint64_t>(driverBacklightFrequency));
			DBGLOG("smoother", "wrapCflRealWriteRegister32: write PWM_DUTY1 0x%x/0x%x, rescaled to 0x%x/0x%x", value, driverBacklightFrequency, rescaledValue, targetBacklightFrequency);
			value = rescaledValue;
		} else {
			// This should never happen, but in case it does we should log it at the very least.
			SYSLOG("smoother", "wrapCflRealWriteRegister32: write PWM_DUTY1 has zero frequency driver (%d) target (%d)", driverBacklightFrequency, targetBacklightFrequency);
		}
	}

	orgWriteRegister32(that, reg, value);
}

void PRODUCT_NAME::wrapCflFakeWriteRegister32(void *that, uint32_t reg, uint32_t value) {
	if (reg == BXT_BLC_PWM_FREQ1) { // aka BLC_PWM_PCH_CTL2
		if (targetBacklightFrequency == 0) {
			// Populate the hardware PWM frequency as initially set up by the system firmware.
			targetBacklightFrequency = orgReadRegister32(that, BXT_BLC_PWM_FREQ1);
			DBGLOG("smoother", "wrapCflFakeWriteRegister32: system initialized BXT_BLC_PWM_FREQ1 = 0x%x", targetBacklightFrequency);
			DBGLOG("smoother", "wrapCflFakeWriteRegister32: system initialized BXT_BLC_PWM_CTL1 = 0x%x", orgReadRegister32(that, BXT_BLC_PWM_CTL1));

			if (targetBacklightFrequency == 0) {
				// This should not happen with correctly written bootloader code, but in case it does, let's use a failsafe default value.
				targetBacklightFrequency = FallbackTargetBacklightFrequency;
				SYSLOG("smoother", "wrapCflFakeWriteRegister32: system initialized BXT_BLC_PWM_FREQ1 is ZERO");
			}
		}

		// For the KBL driver, 0xc8254 (BLC_PWM_PCH_CTL2) controls the backlight intensity.
		// High 16 of this write are the denominator (frequency), low 16 are the numerator (duty cycle).
		// Translate this into a write to c8258 (BXT_BLC_PWM_DUTY1) for the CFL hardware, scaled by the system-provided value in c8254 (BXT_BLC_PWM_FREQ1).
		uint16_t frequency = (value & 0xffff0000U) >> 16U;
		uint16_t dutyCycle = value & 0xffffU;

		uint32_t rescaledValue = frequency == 0 ? 0 : static_cast<uint32_t>((static_cast<uint64_t>(dutyCycle) * static_cast<uint64_t>(targetBacklightFrequency)) / static_cast<uint64_t>(frequency));
		DBGLOG("smoother", "wrapCflFakeWriteRegister32: write PWM_DUTY1 0x%x/0x%x, rescaled to 0x%x/0x%x", dutyCycle, frequency, rescaledValue, targetBacklightFrequency);

		// Reset the hardware PWM frequency. Write the original system value if the driver-requested value is nonzero. If the driver requests
		// zero, we allow that, since it's trying to turn off the backlight PWM for sleep.
		orgWriteRegister32(that, BXT_BLC_PWM_FREQ1, frequency ? targetBacklightFrequency : 0);

		// Finish by writing the duty cycle.
		reg = BXT_BLC_PWM_DUTY1;
		value = rescaledValue;
	} else if (reg == BXT_BLC_PWM_CTL1) {
		if (targetPwmControl == 0) {
			// Save the original hardware PWM control value
			targetPwmControl = orgReadRegister32(that, BXT_BLC_PWM_CTL1);
		}

		DBGLOG("smoother", "wrapCflFakeWriteRegister32: write BXT_BLC_PWM_CTL1 0x%x, previous was 0x%x", value, orgReadRegister32(that, BXT_BLC_PWM_CTL1));

		if (value) {
			// Set the PWM frequency before turning it on to avoid the 3 minute blackout bug
			orgWriteRegister32(that, BXT_BLC_PWM_FREQ1, targetBacklightFrequency);

			// Use the original hardware PWM control value.
			value = targetPwmControl;
		}
	}

	orgWriteRegister32(that, reg, value);
}

static const char *bootargOff[] {
	"-applbklsmoothoff"
};

static const char *bootargDebug[] {
	"-applbklsmoothdbg"
};

static const char *bootargBeta[] {
	"-applbklsmoothbeta"
};

PluginConfiguration ADDPR(config) {
	xStringify(PRODUCT_NAME),
	parseModuleVersion(xStringify(MODULE_VERSION)),
	LiluAPI::AllowNormal | LiluAPI::AllowInstallerRecovery,
	bootargOff,
	arrsize(bootargOff),
	bootargDebug,
	arrsize(bootargDebug),
	bootargBeta,
	arrsize(bootargBeta),
	KernelVersion::MountainLion,
	KernelVersion::BigSur,
	[]() {
		PRODUCT_NAME::init_plugin();
	}
};
