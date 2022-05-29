/* SPDX-License-Identifier: GPL-2.0 */
/* (C) COPYRIGHT 2014-2018 ARM Limited. All rights reserved. */
/* Copyright 2019 Linaro, Ltd., Rob Herring <robh@kernel.org> */
#ifndef __PANFROST_FEATURES_H__
#define __PANFROST_FEATURES_H__

#include <linux/bitops.h>

#include "panfrost_device.h"

enum panfrost_hw_feature {
	HW_FEATURE_JOBCHAIN_DISAMBIGUATION,
	HW_FEATURE_PWRON_DURING_PWROFF_TRANS,
	HW_FEATURE_XAFFINITY,
	HW_FEATURE_V4,
	HW_FEATURE_FLUSH_REDUCTION,
	HW_FEATURE_PROTECTED_MODE,
	HW_FEATURE_COHERENCY_REG,
	HW_FEATURE_PROTECTED_DEBUG_MODE,
	HW_FEATURE_AARCH64_MMU,
	HW_FEATURE_TLS_HASHING,
	HW_FEATURE_THREAD_GROUP_SPLIT,
	HW_FEATURE_IDVS_GROUP_SIZE,
	HW_FEATURE_3BIT_EXT_RW_L2_MMU_CONFIG,
};

#define hw_features_t600 (\
	BIT_ULL(HW_FEATURE_THREAD_GROUP_SPLIT) | \
	BIT_ULL(HW_FEATURE_V4))

#define hw_features_t620 hw_features_t600

#define hw_features_t720 hw_features_t600

#define hw_features_t760 (\
	BIT_ULL(HW_FEATURE_JOBCHAIN_DISAMBIGUATION) | \
	BIT_ULL(HW_FEATURE_PWRON_DURING_PWROFF_TRANS) | \
	BIT_ULL(HW_FEATURE_XAFFINITY) | \
	BIT_ULL(HW_FEATURE_THREAD_GROUP_SPLIT))

#define hw_features_t860 hw_features_t760

#define hw_features_t880 hw_features_t760

#define hw_features_t830 hw_features_t760

#define hw_features_t820 hw_features_t760

#define hw_features_g71 (\
	BIT_ULL(HW_FEATURE_JOBCHAIN_DISAMBIGUATION) | \
	BIT_ULL(HW_FEATURE_PWRON_DURING_PWROFF_TRANS) | \
	BIT_ULL(HW_FEATURE_XAFFINITY) | \
	BIT_ULL(HW_FEATURE_THREAD_GROUP_SPLIT) | \
	BIT_ULL(HW_FEATURE_FLUSH_REDUCTION) | \
	BIT_ULL(HW_FEATURE_PROTECTED_MODE) | \
	BIT_ULL(HW_FEATURE_COHERENCY_REG))

#define hw_features_g72 (\
	BIT_ULL(HW_FEATURE_JOBCHAIN_DISAMBIGUATION) | \
	BIT_ULL(HW_FEATURE_PWRON_DURING_PWROFF_TRANS) | \
	BIT_ULL(HW_FEATURE_XAFFINITY) | \
	BIT_ULL(HW_FEATURE_THREAD_GROUP_SPLIT) | \
	BIT_ULL(HW_FEATURE_FLUSH_REDUCTION) | \
	BIT_ULL(HW_FEATURE_PROTECTED_MODE) | \
	BIT_ULL(HW_FEATURE_PROTECTED_DEBUG_MODE) | \
	BIT_ULL(HW_FEATURE_COHERENCY_REG))

#define hw_features_g51 hw_features_g72

#define hw_features_g52 (\
	BIT_ULL(HW_FEATURE_JOBCHAIN_DISAMBIGUATION) | \
	BIT_ULL(HW_FEATURE_PWRON_DURING_PWROFF_TRANS) | \
	BIT_ULL(HW_FEATURE_XAFFINITY) | \
	BIT_ULL(HW_FEATURE_THREAD_GROUP_SPLIT) | \
	BIT_ULL(HW_FEATURE_FLUSH_REDUCTION) | \
	BIT_ULL(HW_FEATURE_PROTECTED_MODE) | \
	BIT_ULL(HW_FEATURE_PROTECTED_DEBUG_MODE) | \
	BIT_ULL(HW_FEATURE_IDVS_GROUP_SIZE) | \
	BIT_ULL(HW_FEATURE_COHERENCY_REG))

#define hw_features_g76 (\
	BIT_ULL(HW_FEATURE_JOBCHAIN_DISAMBIGUATION) | \
	BIT_ULL(HW_FEATURE_PWRON_DURING_PWROFF_TRANS) | \
	BIT_ULL(HW_FEATURE_XAFFINITY) | \
	BIT_ULL(HW_FEATURE_THREAD_GROUP_SPLIT) | \
	BIT_ULL(HW_FEATURE_FLUSH_REDUCTION) | \
	BIT_ULL(HW_FEATURE_PROTECTED_MODE) | \
	BIT_ULL(HW_FEATURE_PROTECTED_DEBUG_MODE) | \
	BIT_ULL(HW_FEATURE_COHERENCY_REG) | \
	BIT_ULL(HW_FEATURE_AARCH64_MMU) | \
	BIT_ULL(HW_FEATURE_TLS_HASHING) | \
	BIT_ULL(HW_FEATURE_IDVS_GROUP_SIZE) | \
	BIT_ULL(HW_FEATURE_3BIT_EXT_RW_L2_MMU_CONFIG))

#define hw_features_g31 (\
	BIT_ULL(HW_FEATURE_JOBCHAIN_DISAMBIGUATION) | \
	BIT_ULL(HW_FEATURE_PWRON_DURING_PWROFF_TRANS) | \
	BIT_ULL(HW_FEATURE_XAFFINITY) | \
	BIT_ULL(HW_FEATURE_THREAD_GROUP_SPLIT) | \
	BIT_ULL(HW_FEATURE_FLUSH_REDUCTION) | \
	BIT_ULL(HW_FEATURE_PROTECTED_MODE) | \
	BIT_ULL(HW_FEATURE_PROTECTED_DEBUG_MODE) | \
	BIT_ULL(HW_FEATURE_COHERENCY_REG) | \
	BIT_ULL(HW_FEATURE_AARCH64_MMU) | \
	BIT_ULL(HW_FEATURE_TLS_HASHING) | \
	BIT_ULL(HW_FEATURE_3BIT_EXT_RW_L2_MMU_CONFIG))

static inline bool panfrost_has_hw_feature(struct panfrost_device *pfdev,
					   enum panfrost_hw_feature feat)
{
	return test_bit(feat, pfdev->features.hw_features);
}

#endif
