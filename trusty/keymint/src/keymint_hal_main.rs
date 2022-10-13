//
// Copyright (C) 2022 The Android Open-Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

//! This module implements the HAL service for Keymint (Rust) in Trusty.
use kmr_hal::{keymint, rpc, secureclock, send_hal_info, sharedsecret, SerializedChannel};
use log::{debug, error, info};
use std::{
    ffi::CString,
    ops::DerefMut,
    panic,
    sync::{Arc, Mutex},
};
use trusty::DEFAULT_DEVICE;

const TRUSTY_KEYMINT_RUST_SERVICE_NAME: &str = "com.android.trusty.keymint";

// TODO(b/195310053): This HAL service currently runs as a parallel /rust instance of the HAL
// service(s), to allow development and testing of the Rust reference implementation of KeyMint.
// Once the Rust version becomes the default, this should become "default".
static SERVICE_INSTANCE: &str = "rust";

static KM_SERVICE_NAME: &str = "android.hardware.security.keymint.IKeyMintDevice";
static RPC_SERVICE_NAME: &str = "android.hardware.security.keymint.IRemotelyProvisionedComponent";
static SECURE_CLOCK_SERVICE_NAME: &str = "android.hardware.security.secureclock.ISecureClock";
static SHARED_SECRET_SERVICE_NAME: &str = "android.hardware.security.sharedsecret.ISharedSecret";

#[derive(Debug)]
struct TipcChannel(trusty::TipcChannel);

impl SerializedChannel for TipcChannel {
    const MAX_SIZE: usize = 4000;
    fn execute(&mut self, serialized_req: &[u8]) -> binder::Result<Vec<u8>> {
        self.0.send(serialized_req).map_err(|e| {
            binder::Status::new_exception(
                binder::ExceptionCode::TRANSACTION_FAILED,
                Some(
                    &CString::new(format!(
                        "Failed to send the request via tipc channel because of {:?}",
                        e
                    ))
                    .unwrap(),
                ),
            )
        })?;
        let mut expect_more_msgs = true;
        let mut full_rsp = Vec::new();
        while expect_more_msgs {
            let mut recv_buf = Vec::new();
            self.0.recv(&mut recv_buf).map_err(|e| {
                binder::Status::new_exception(
                    binder::ExceptionCode::TRANSACTION_FAILED,
                    Some(
                        &CString::new(format!(
                            "Failed to receive the response via tipc channel because of {:?}",
                            e
                        ))
                        .unwrap(),
                    ),
                )
            })?;
            let current_rsp_content;
            (expect_more_msgs, current_rsp_content) = Self::handle_resp_received(recv_buf);
            debug!(
                "In execute: expect more messages: {}, Processed current respone size {}",
                expect_more_msgs,
                current_rsp_content.len()
            );
            full_rsp.extend_from_slice(&current_rsp_content);
            debug!("In execute: Processed full response size yet: {}", full_rsp.len())
        }
        debug!("In execute: Full response size: {}", full_rsp.len());
        Ok(full_rsp)
    }
}

fn main() {
    // Initialize Android logging.
    android_logger::init_once(
        android_logger::Config::default()
            .with_tag("keymint-hal-trusty")
            .with_min_level(log::Level::Debug)
            .with_log_id(android_logger::LogId::System),
    );
    // Redirect panic messages to logcat.
    panic::set_hook(Box::new(|panic_info| {
        error!("{}", panic_info);
    }));

    info!("Trusty KM HAL service is starting.");

    info!("Starting thread pool now.");
    binder::ProcessState::start_thread_pool();

    // Create connection to the TA
    let connection = trusty::TipcChannel::connect(DEFAULT_DEVICE, TRUSTY_KEYMINT_RUST_SERVICE_NAME)
        .unwrap_or_else(|e| panic!("Failed to connect to Trusty Keymint TA because of {:?}.", e));
    let tipc_channel = Arc::new(Mutex::new(TipcChannel(connection)));

    // Register the Keymint service
    let km_service = keymint::Device::new_as_binder(tipc_channel.clone());
    let km_service_name = format!("{}/{}", KM_SERVICE_NAME, SERVICE_INSTANCE);
    binder::add_service(&km_service_name, km_service.as_binder()).unwrap_or_else(|e| {
        panic!("Failed to register service {} because of {:?}.", km_service_name, e);
    });

    // Register the Remotely Provisioned Component service
    let rpc_service = rpc::Device::new_as_binder(tipc_channel.clone());
    let rpc_service_name = format!("{}/{}", RPC_SERVICE_NAME, SERVICE_INSTANCE);
    binder::add_service(&rpc_service_name, rpc_service.as_binder()).unwrap_or_else(|e| {
        panic!("Failed to register service {} because of {:?}.", rpc_service_name, e);
    });

    // Register the Secure Clock service
    let sclock_service = secureclock::Device::new_as_binder(tipc_channel.clone());
    let sclock_service_name = format!("{}/{}", SECURE_CLOCK_SERVICE_NAME, SERVICE_INSTANCE);
    binder::add_service(&sclock_service_name, sclock_service.as_binder()).unwrap_or_else(|e| {
        panic!("Failed to register service {} because of {:?}.", sclock_service_name, e);
    });

    // Register the Shared Secret service
    let ssecret_service = sharedsecret::Device::new_as_binder(tipc_channel.clone());
    let ssecret_service_name = format!("{}/{}", SHARED_SECRET_SERVICE_NAME, SERVICE_INSTANCE);
    binder::add_service(&ssecret_service_name, ssecret_service.as_binder()).unwrap_or_else(|e| {
        panic!("Failed to register service {} because of {:?}.", ssecret_service_name, e);
    });

    // Send the HAL service information to the TA
    if let Err(e) = send_hal_info(tipc_channel.lock().unwrap().deref_mut()) {
        error!("Failed to populate HAL info: {:?}", e);
    }

    info!("Successfully registered KeyMint HAL services.");
    info!("Joining thread pool now.");
    binder::ProcessState::join_thread_pool();
    info!("KeyMint HAL service is terminating."); // should not reach here
}
