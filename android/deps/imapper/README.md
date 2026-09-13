# Android IMapper stable C declarations

`IMapper.h` comes from AOSP hardware/interfaces commit
`5688f7eb1e117ed26e642a695de300b7683acb87`, at
`graphics/mapper/stable-c/include/android/hardware/graphics/mapper/IMapper.h`.
It retains its Apache-2.0 license. Local changes add the explicit `stddef.h`
include and reference the adjacent native-handle declaration.

`native_handle.h` is the Apache-2.0 Android system/core declaration distributed
with arlinux's Android headers. Only its native-handle types are used here.

The metadata decoder follows the same revision's
`graphics/mapper/stable-c/implutils/include/android/hardware/graphics/mapper/utils/IMapperMetadataTypes.h`
and `graphics/common/aidl/android/hardware/graphics/common/StandardMetadataType.aidl`.
It accepts only bounded, single-plane, uncompressed RGBA/BGRA layouts. It does
not decode private Qualcomm handle fields.

The compositor reopens the already loaded `mapper.*.so` through
`android_load_sphal_library` with `RTLD_NOLOAD`. A device without a unique
loaded stable-C mapper simply supplies no layout event. Existing protocol
clients and legacy mappers retain their previous behavior.
