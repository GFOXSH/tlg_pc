# Project-maintained FSR4 v07 provider

This directory vendors the FSR4 v07 Vulkan provider and its self-contained public
headers from FireBurn/UniversalScalers-Vulkan at
`c64f093404125e960813f95f1996c14780f0dd69` (the revision recorded by the
former `port/gpu/third_party/fsr-vulkan` gitlink).

The provider is built from these project-owned files. The build does not compile
or modify the FSR4 provider sources from the FSR-Vulkan submodule, and no patch
is applied to a submodule working tree. This keeps the TLG FSR4 integration,
including `TLG_FSR4_PROFILE` pass timings and `TLG_FSR4_STATS` pipeline
statistics, independent of changes to the submodule's provider implementation.

The original FSR-Vulkan MIT license is included in `LICENSE.txt`. The local
provider source carries TLG's optional profiling/statistics instrumentation.
The FSR3 portable SDK remains supplied by the submodule and is not replaced by
this vendor copy.
