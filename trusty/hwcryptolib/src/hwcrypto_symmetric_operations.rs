/*
 * Copyright (C) 2023 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//! Implementation of the `IHwCryptoKeySymmetricOperations` AIDL interface. It contains all the
//! symmetric cryptography functionality.

use android_hardware_security_see::aidl::android::hardware::security::see::hwcrypto::HwCryptoKeyMaterial::HwCryptoKeyMaterial;
use android_hardware_security_see::aidl::android::hardware::security::see::hwcrypto::IHwCryptoKeySymmetricOperations::BnHwCryptoKeySymmetricOperations;
use android_hardware_security_see::aidl::android::hardware::security::see::hwcrypto::IHwCryptoKeySymmetricOperations::IHwCryptoKeySymmetricOperations;
use android_hardware_security_see::aidl::android::hardware::security::see::hwcrypto::base_types::AeadOperationResult::AeadOperationResult;
use android_hardware_security_see::aidl::android::hardware::security::see::hwcrypto::base_types::DmaAeadOperationResult::DmaAeadOperationResult;
use android_hardware_security_see::aidl::android::hardware::security::see::hwcrypto::base_types::DmaOperationBuffers::DmaOperationBuffers;
use android_hardware_security_see::aidl::android::hardware::security::see::hwcrypto::base_types::DmaEmittingOperationResult::DmaEmittingOperationResult;
use android_hardware_security_see::aidl::android::hardware::security::see::hwcrypto::base_types::EmittingOperationResult::EmittingOperationResult;
use android_hardware_security_see::aidl::android::hardware::security::see::hwcrypto::base_types::SymmetricOperationParameters::SymmetricOperationParameters;

use android_hardware_security_see::binder;

use crate::symmetric_aead_operation::SymmetricAeadOperation;
use crate::symmetric_dma_aead_operation::SymmetricDmaAeadOperation;
use crate::symmetric_dma_emitting_operation::SymmetricDmaEmittingOperation;
use crate::symmetric_emitting_operation::SymmetricEmittingOperation;

/// The `IHwCryptoKeySymmetricOperations` implementation.
pub struct HwCryptoKeySymmetricOperations;

impl HwCryptoKeySymmetricOperations {
    pub(crate) fn new_operation(
    ) -> binder::Result<binder::Strong<dyn IHwCryptoKeySymmetricOperations>> {
        let hwcryptokey_symmetric_operations = HwCryptoKeySymmetricOperations;
        let hwcryptokey_symmetric_operations_binder = BnHwCryptoKeySymmetricOperations::new_binder(
            hwcryptokey_symmetric_operations,
            binder::BinderFeatures::default(),
        );
        Ok(hwcryptokey_symmetric_operations_binder)
    }
}

impl binder::Interface for HwCryptoKeySymmetricOperations {}

impl IHwCryptoKeySymmetricOperations for HwCryptoKeySymmetricOperations {
    fn begin(
        &self,
        key: &HwCryptoKeyMaterial,
        parameters: &SymmetricOperationParameters,
    ) -> binder::Result<EmittingOperationResult> {
        SymmetricEmittingOperation::new_operation(key, parameters)
    }

    fn begin_aead(
        &self,
        key: &HwCryptoKeyMaterial,
        parameters: &SymmetricOperationParameters,
    ) -> binder::Result<AeadOperationResult> {
        SymmetricAeadOperation::new_operation(key, parameters)
    }

    fn begin_dma(
        &self,
        key: &HwCryptoKeyMaterial,
        parameters: &SymmetricOperationParameters,
        dma_buffers: &DmaOperationBuffers,
    ) -> binder::Result<DmaEmittingOperationResult> {
        SymmetricDmaEmittingOperation::new_operation(key, parameters, dma_buffers)
    }

    fn begin_dma_aead(
        &self,
        key: &HwCryptoKeyMaterial,
        parameters: &SymmetricOperationParameters,
        dma_buffers: &DmaOperationBuffers,
    ) -> binder::Result<DmaAeadOperationResult> {
        SymmetricDmaAeadOperation::new_operation(key, parameters, dma_buffers)
    }
}
