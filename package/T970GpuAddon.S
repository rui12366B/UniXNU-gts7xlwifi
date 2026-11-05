// gts7xlwifi: the GPU addon (scripts/make-gpu-addon.py output: the NativeAdrenoMetal plugin bundle with Turnip, adrenoprobed and
// its job plists) carried in the kernel image, because this board's DarwinBoot hands no /chosen/memory-map/GpuAddon over.
// kern_exec.c's addon install falls back to these two symbols (patches/xnu/t970-gpu-addon-embedded.patch). The file is
// assembled by scripts/build-adreno650.py with T970_GPU_ADDON set to the addon's path.
	// __DATA, not __DATA_CONST: __DATA_CONST lies before __TEXT_EXEC, and 13 MiB there put the kernel code out of BL range
	// (+/-128 MiB) of the prelinked kexts below it (v41 link: arm64_b26 fixup out of range).
	.section __DATA,__t970_gpu_addon
	.p2align 3
	.globl _t970_gpu_addon_data
_t970_gpu_addon_data:
	.incbin T970_GPU_ADDON
_t970_gpu_addon_end:
	.p2align 3
	.globl _t970_gpu_addon_size
_t970_gpu_addon_size:
	.quad _t970_gpu_addon_end - _t970_gpu_addon_data
