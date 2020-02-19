/*
 * Copyright 2019 The Android Open Source Project
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

#define LOG_TAG "bt_shim_btm"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>

#include "main/shim/btm.h"
#include "main/shim/controller.h"
#include "main/shim/entry.h"
#include "main/shim/shim.h"
#include "osi/include/log.h"
#include "stack/btm/btm_int_types.h"
#include "types/class_of_device.h"
#include "types/raw_address.h"

#include "hci/le_scanning_manager.h"
#include "main/shim/helpers.h"
#include "neighbor/connectability.h"
#include "neighbor/discoverability.h"
#include "neighbor/name.h"
#include "neighbor/page.h"
#include "security/security_module.h"
#include "shim/advertising.h"
#include "shim/controller.h"
#include "shim/inquiry.h"

extern tBTM_CB btm_cb;

static constexpr size_t kRemoteDeviceNameLength = 248;

static constexpr uint8_t kAdvDataInfoNotPresent = 0xff;
static constexpr uint8_t kTxPowerInformationNotPresent = 0x7f;
static constexpr uint8_t kNotPeriodicAdvertisement = 0x00;

static constexpr bool kActiveScanning = true;
static constexpr bool kPassiveScanning = false;

using BtmRemoteDeviceName = tBTM_REMOTE_DEV_NAME;

extern void btm_process_cancel_complete(uint8_t status, uint8_t mode);
extern void btm_process_inq_complete(uint8_t status, uint8_t result_type);
extern void btm_ble_process_adv_addr(RawAddress& raw_address,
                                     uint8_t* address_type);
extern void btm_ble_process_adv_pkt_cont(
    uint16_t event_type, uint8_t address_type, const RawAddress& raw_address,
    uint8_t primary_phy, uint8_t secondary_phy, uint8_t advertising_sid,
    int8_t tx_power, int8_t rssi, uint16_t periodic_adv_int, uint8_t data_len,
    uint8_t* data);

extern void btm_api_process_inquiry_result(const RawAddress& raw_address,
                                           uint8_t page_scan_rep_mode,
                                           DEV_CLASS device_class,
                                           uint16_t clock_offset);

extern void btm_api_process_inquiry_result_with_rssi(RawAddress raw_address,
                                                     uint8_t page_scan_rep_mode,
                                                     DEV_CLASS device_class,
                                                     uint16_t clock_offset,
                                                     int8_t rssi);

extern void btm_api_process_extended_inquiry_result(
    RawAddress raw_address, uint8_t page_scan_rep_mode, DEV_CLASS device_class,
    uint16_t clock_offset, int8_t rssi, const uint8_t* eir_data,
    size_t eir_len);

void bluetooth::shim::Btm::StartUp(bluetooth::shim::Btm* btm) {
  CHECK(btm != nullptr);
  std::unique_lock<std::mutex> lock(btm->sync_mutex_);
  CHECK(btm->observing_timer_ == nullptr);
  CHECK(btm->scanning_timer_ == nullptr);
  btm->observing_timer_ = new bluetooth::shim::Timer("observing_timer");
  btm->scanning_timer_ = new bluetooth::shim::Timer("scanning_timer");
}

void bluetooth::shim::Btm::ShutDown(bluetooth::shim::Btm* btm) {
  CHECK(btm != nullptr);
  std::unique_lock<std::mutex> lock(btm->sync_mutex_);
  CHECK(btm->observing_timer_ != nullptr);
  CHECK(btm->scanning_timer_ != nullptr);
  delete btm->scanning_timer_;
  delete btm->observing_timer_;
  btm->scanning_timer_ = nullptr;
  btm->observing_timer_ = nullptr;
}

void bluetooth::shim::Btm::OnInquiryResult(std::string string_address,
                                           uint8_t page_scan_rep_mode,
                                           std::string string_class_of_device,
                                           uint16_t clock_offset) {
  RawAddress raw_address;
  RawAddress::FromString(string_address, raw_address);
  ClassOfDevice class_of_device;
  ClassOfDevice::FromString(string_class_of_device, class_of_device);

  btm_api_process_inquiry_result(raw_address, page_scan_rep_mode,
                                 class_of_device.cod, clock_offset);
}

void bluetooth::shim::Btm::OnInquiryResultWithRssi(
    std::string string_address, uint8_t page_scan_rep_mode,
    std::string string_class_of_device, uint16_t clock_offset, int8_t rssi) {
  RawAddress raw_address;
  RawAddress::FromString(string_address, raw_address);
  ClassOfDevice class_of_device;
  ClassOfDevice::FromString(string_class_of_device, class_of_device);

  btm_api_process_inquiry_result_with_rssi(
      raw_address, page_scan_rep_mode, class_of_device.cod, clock_offset, rssi);
}

void bluetooth::shim::Btm::OnExtendedInquiryResult(
    std::string string_address, uint8_t page_scan_rep_mode,
    std::string string_class_of_device, uint16_t clock_offset, int8_t rssi,
    const uint8_t* gap_data, size_t gap_data_len) {
  RawAddress raw_address;
  RawAddress::FromString(string_address, raw_address);
  ClassOfDevice class_of_device;
  ClassOfDevice::FromString(string_class_of_device, class_of_device);

  btm_api_process_extended_inquiry_result(raw_address, page_scan_rep_mode,
                                          class_of_device.cod, clock_offset,
                                          rssi, gap_data, gap_data_len);
}

void bluetooth::shim::Btm::OnInquiryComplete(uint16_t status) {
  legacy_inquiry_complete_callback_(
      (status == 0) ? (BTM_SUCCESS) : (BTM_ERR_PROCESSING),
      active_inquiry_mode_);
  active_inquiry_mode_ = kInquiryModeOff;
}

bool bluetooth::shim::Btm::SetInquiryFilter(uint8_t mode, uint8_t type,
                                            tBTM_INQ_FILT_COND data) {
  switch (mode) {
    case kInquiryModeOff:
      break;
    case kLimitedInquiryMode:
      LOG_WARN(LOG_TAG, "UNIMPLEMENTED %s", __func__);
      break;
    case kGeneralInquiryMode:
      LOG_WARN(LOG_TAG, "UNIMPLEMENTED %s", __func__);
      break;
    default:
      LOG_WARN(LOG_TAG, "%s Unknown inquiry mode:%d", __func__, mode);
      return false;
  }
  return true;
}

void bluetooth::shim::Btm::SetFilterInquiryOnAddress() {
  LOG_WARN(LOG_TAG, "UNIMPLEMENTED %s", __func__);
}

void bluetooth::shim::Btm::SetFilterInquiryOnDevice() {
  LOG_WARN(LOG_TAG, "UNIMPLEMENTED %s", __func__);
}

void bluetooth::shim::Btm::ClearInquiryFilter() {
  LOG_WARN(LOG_TAG, "UNIMPLEMENTED %s", __func__);
}

void bluetooth::shim::Btm::SetStandardInquiryResultMode() {
  bluetooth::shim::GetInquiry()->SetStandardInquiryResultMode();
}

void bluetooth::shim::Btm::SetInquiryWithRssiResultMode() {
  bluetooth::shim::GetInquiry()->SetInquiryWithRssiResultMode();
}

void bluetooth::shim::Btm::SetExtendedInquiryResultMode() {
  bluetooth::shim::GetInquiry()->SetExtendedInquiryResultMode();
}

void bluetooth::shim::Btm::SetInterlacedInquiryScan() {
  bluetooth::shim::GetInquiry()->SetInterlacedScan();
}

void bluetooth::shim::Btm::SetStandardInquiryScan() {
  bluetooth::shim::GetInquiry()->SetStandardScan();
}

bool bluetooth::shim::Btm::IsInterlacedScanSupported() const {
  return controller_get_interface()->supports_interlaced_inquiry_scan();
}

/**
 * One shot inquiry
 */
bool bluetooth::shim::Btm::StartInquiry(
    uint8_t mode, uint8_t duration, uint8_t max_responses,
    LegacyInquiryCompleteCallback legacy_inquiry_complete_callback) {
  switch (mode) {
    case kInquiryModeOff:
      LOG_DEBUG(LOG_TAG, "%s Stopping inquiry mode", __func__);
      bluetooth::shim::GetInquiry()->StopInquiry();
      active_inquiry_mode_ = kInquiryModeOff;
      break;

    case kLimitedInquiryMode:
    case kGeneralInquiryMode: {
      LegacyInquiryCallbacks legacy_inquiry_callbacks{
          .result_callback =
              std::bind(&Btm::OnInquiryResult, this, std::placeholders::_1,
                        std::placeholders::_2, std::placeholders::_3,
                        std::placeholders::_4),
          .result_with_rssi_callback = std::bind(
              &Btm::OnInquiryResultWithRssi, this, std::placeholders::_1,
              std::placeholders::_2, std::placeholders::_3,
              std::placeholders::_4, std::placeholders::_5),
          .extended_result_callback = std::bind(
              &Btm::OnExtendedInquiryResult, this, std::placeholders::_1,
              std::placeholders::_2, std::placeholders::_3,
              std::placeholders::_4, std::placeholders::_5,
              std::placeholders::_6, std::placeholders::_7),
          .complete_callback =
              std::bind(&Btm::OnInquiryComplete, this, std::placeholders::_1),
      };

      if (mode == kLimitedInquiryMode) {
        LOG_DEBUG(
            LOG_TAG,
            "%s Starting limited inquiry mode duration:%hhd max responses:%hhd",
            __func__, duration, max_responses);
        bluetooth::shim::GetInquiry()->StartLimitedInquiry(
            duration, max_responses, legacy_inquiry_callbacks);
        active_inquiry_mode_ = kLimitedInquiryMode;
      } else {
        LOG_DEBUG(
            LOG_TAG,
            "%s Starting general inquiry mode duration:%hhd max responses:%hhd",
            __func__, duration, max_responses);
        bluetooth::shim::GetInquiry()->StartGeneralInquiry(
            duration, max_responses, legacy_inquiry_callbacks);
        legacy_inquiry_complete_callback_ = legacy_inquiry_complete_callback;
      }
    } break;

    default:
      LOG_WARN(LOG_TAG, "%s Unknown inquiry mode:%d", __func__, mode);
      return false;
  }
  return true;
}

void bluetooth::shim::Btm::CancelInquiry() {
  LOG_DEBUG(LOG_TAG, "%s", __func__);
  bluetooth::shim::GetInquiry()->StopInquiry();
}

bool bluetooth::shim::Btm::IsInquiryActive() const {
  return IsGeneralInquiryActive() || IsLimitedInquiryActive();
}

bool bluetooth::shim::Btm::IsGeneralInquiryActive() const {
  return bluetooth::shim::GetInquiry()->IsGeneralInquiryActive();
}

bool bluetooth::shim::Btm::IsLimitedInquiryActive() const {
  return bluetooth::shim::GetInquiry()->IsLimitedInquiryActive();
}

/**
 * Periodic
 */
bool bluetooth::shim::Btm::StartPeriodicInquiry(
    uint8_t mode, uint8_t duration, uint8_t max_responses, uint16_t max_delay,
    uint16_t min_delay, tBTM_INQ_RESULTS_CB* p_results_cb) {
  switch (mode) {
    case kInquiryModeOff:
      bluetooth::shim::GetInquiry()->StopPeriodicInquiry();
      break;

    case kLimitedInquiryMode:
    case kGeneralInquiryMode: {
      LegacyInquiryCallbacks legacy_inquiry_callbacks{
          .result_callback =
              std::bind(&Btm::OnInquiryResult, this, std::placeholders::_1,
                        std::placeholders::_2, std::placeholders::_3,
                        std::placeholders::_4),
          .result_with_rssi_callback = std::bind(
              &Btm::OnInquiryResultWithRssi, this, std::placeholders::_1,
              std::placeholders::_2, std::placeholders::_3,
              std::placeholders::_4, std::placeholders::_5),
          .extended_result_callback = std::bind(
              &Btm::OnExtendedInquiryResult, this, std::placeholders::_1,
              std::placeholders::_2, std::placeholders::_3,
              std::placeholders::_4, std::placeholders::_5,
              std::placeholders::_6, std::placeholders::_7),
          .complete_callback =
              std::bind(&Btm::OnInquiryComplete, this, std::placeholders::_1),
      };
      if (mode == kLimitedInquiryMode) {
        LOG_DEBUG(LOG_TAG, "%s Starting limited periodic inquiry mode",
                  __func__);
        bluetooth::shim::GetInquiry()->StartLimitedPeriodicInquiry(
            duration, max_responses, max_delay, min_delay,
            legacy_inquiry_callbacks);
      } else {
        LOG_DEBUG(LOG_TAG, "%s Starting general periodic inquiry mode",
                  __func__);
        bluetooth::shim::GetInquiry()->StartGeneralPeriodicInquiry(
            duration, max_responses, max_delay, min_delay,
            legacy_inquiry_callbacks);
      }
    } break;

    default:
      LOG_WARN(LOG_TAG, "%s Unknown inquiry mode:%d", __func__, mode);
      return false;
  }
  return true;
}

void bluetooth::shim::Btm::CancelPeriodicInquiry() {
  bluetooth::shim::GetInquiry()->StopPeriodicInquiry();
}

bool bluetooth::shim::Btm::IsGeneralPeriodicInquiryActive() const {
  return bluetooth::shim::GetInquiry()->IsGeneralPeriodicInquiryActive();
}

bool bluetooth::shim::Btm::IsLimitedPeriodicInquiryActive() const {
  return bluetooth::shim::GetInquiry()->IsLimitedPeriodicInquiryActive();
}

/**
 * Discoverability
 */
void bluetooth::shim::Btm::SetClassicGeneralDiscoverability(uint16_t window,
                                                            uint16_t interval) {
  bluetooth::shim::GetInquiry()->SetScanActivity(interval, window);
  bluetooth::shim::GetDiscoverability()->StartGeneralDiscoverability();
}

void bluetooth::shim::Btm::SetClassicLimitedDiscoverability(uint16_t window,
                                                            uint16_t interval) {
  bluetooth::shim::GetInquiry()->SetScanActivity(interval, window);
  bluetooth::shim::GetDiscoverability()->StartLimitedDiscoverability();
}

void bluetooth::shim::Btm::SetClassicDiscoverabilityOff() {
  bluetooth::shim::GetDiscoverability()->StopDiscoverability();
}

DiscoverabilityState bluetooth::shim::Btm::GetClassicDiscoverabilityState()
    const {
  DiscoverabilityState state{.mode = BTM_NON_DISCOVERABLE};
  bluetooth::shim::GetInquiry()->GetScanActivity(state.interval, state.window);

  if (bluetooth::shim::GetDiscoverability()
          ->IsGeneralDiscoverabilityEnabled()) {
    state.mode = BTM_GENERAL_DISCOVERABLE;
  } else if (bluetooth::shim::GetDiscoverability()
                 ->IsLimitedDiscoverabilityEnabled()) {
    state.mode = BTM_LIMITED_DISCOVERABLE;
  }
  return state;
}

void bluetooth::shim::Btm::SetLeGeneralDiscoverability() {
  LOG_WARN(LOG_TAG, "UNIMPLEMENTED %s", __func__);
}

void bluetooth::shim::Btm::SetLeLimitedDiscoverability() {
  LOG_WARN(LOG_TAG, "UNIMPLEMENTED %s", __func__);
}

void bluetooth::shim::Btm::SetLeDiscoverabilityOff() {
  LOG_WARN(LOG_TAG, "UNIMPLEMENTED %s", __func__);
}

DiscoverabilityState bluetooth::shim::Btm::GetLeDiscoverabilityState() const {
  DiscoverabilityState state{
      .mode = kDiscoverableModeOff,
      .interval = 0,
      .window = 0,
  };
  LOG_WARN(LOG_TAG, "UNIMPLEMENTED %s", __func__);
  return state;
}

/**
 * Connectability
 */
void bluetooth::shim::Btm::SetClassicConnectibleOn() {
  bluetooth::shim::GetConnectability()->StartConnectability();
}

void bluetooth::shim::Btm::SetClassicConnectibleOff() {
  bluetooth::shim::GetConnectability()->StopConnectability();
}

ConnectabilityState bluetooth::shim::Btm::GetClassicConnectabilityState()
    const {
  ConnectabilityState state;
  neighbor::ScanParameters parameters =
      bluetooth::shim::GetPage()->GetScanActivity();
  state.interval = parameters.interval;
  state.window = parameters.window;

  if (bluetooth::shim::GetConnectability()->IsConnectable()) {
    state.mode = BTM_CONNECTABLE;
  } else {
    state.mode = BTM_NON_CONNECTABLE;
  }
  return state;
}

void bluetooth::shim::Btm::SetInterlacedPageScan() {
  bluetooth::shim::GetPage()->SetInterlacedScan();
}

void bluetooth::shim::Btm::SetStandardPageScan() {
  bluetooth::shim::GetPage()->SetStandardScan();
}

void bluetooth::shim::Btm::SetLeConnectibleOn() {
  LOG_WARN(LOG_TAG, "UNIMPLEMENTED %s", __func__);
}

void bluetooth::shim::Btm::SetLeConnectibleOff() {
  LOG_WARN(LOG_TAG, "UNIMPLEMENTED %s", __func__);
}

ConnectabilityState bluetooth::shim::Btm::GetLeConnectabilityState() const {
  ConnectabilityState state{
      .mode = kConnectibleModeOff,
      .interval = 0,
      .window = 0,
  };
  LOG_WARN(LOG_TAG, "UNIMPLEMENTED %s", __func__);
  return state;
}

bool bluetooth::shim::Btm::IsLeAclConnected(
    const RawAddress& raw_address) const {
  // TODO(cmanton) Check current acl's for this address and indicate if there is
  // an LE option.  For now ignore and default to classic.
  LOG_INFO(LOG_TAG, "%s Le acl connection check is temporarily unsupported",
           __func__);
  return false;
}

bluetooth::shim::BtmStatus bluetooth::shim::Btm::ReadClassicRemoteDeviceName(
    const RawAddress& raw_address, tBTM_CMPL_CB* callback) {
  if (!CheckClassicAclLink(raw_address)) {
    return bluetooth::shim::BTM_UNKNOWN_ADDR;
  }

  if (!classic_read_remote_name_.Start(raw_address)) {
    LOG_INFO(LOG_TAG, "%s Read remote name is currently busy address:%s",
             __func__, raw_address.ToString().c_str());
    return bluetooth::shim::BTM_BUSY;
  }

  LOG_DEBUG(LOG_TAG, "%s Start read name from address:%s", __func__,
            raw_address.ToString().c_str());
  bluetooth::shim::GetName()->ReadRemoteNameRequest(
      hci::Address(raw_address.address), hci::PageScanRepetitionMode::R1,
      0 /* clock_offset */, hci::ClockOffsetValid::INVALID,

      base::Bind(
          [](tBTM_CMPL_CB* callback, ReadRemoteName* classic_read_remote_name_,
             hci::ErrorCode status, hci::Address address,
             std::array<uint8_t, kRemoteDeviceNameLength> remote_name) {
            RawAddress raw_address(address.address);

            BtmRemoteDeviceName name{
                .status = (static_cast<uint8_t>(status) == 0)
                              ? (BTM_SUCCESS)
                              : (BTM_BAD_VALUE_RET),
                .bd_addr = raw_address,
                .length = kRemoteDeviceNameLength,
            };
            std::copy(remote_name.begin(), remote_name.end(),
                      name.remote_bd_name);
            LOG_DEBUG(LOG_TAG, "%s Finish read name from address:%s name:%s",
                      __func__, address.ToString().c_str(),
                      name.remote_bd_name);
            callback(&name);
            classic_read_remote_name_->Stop();
          },
          callback, &classic_read_remote_name_),
      bluetooth::shim::GetGdShimHandler());
  return bluetooth::shim::BTM_CMD_STARTED;
}

bluetooth::shim::BtmStatus bluetooth::shim::Btm::ReadLeRemoteDeviceName(
    const RawAddress& raw_address, tBTM_CMPL_CB* callback) {
  if (!CheckLeAclLink(raw_address)) {
    return bluetooth::shim::BTM_UNKNOWN_ADDR;
  }

  if (!le_read_remote_name_.Start(raw_address)) {
    return bluetooth::shim::BTM_BUSY;
  }

  LOG_INFO(LOG_TAG, "UNIMPLEMENTED %s need access to GATT module", __func__);
  return bluetooth::shim::BTM_UNKNOWN_ADDR;
}

bluetooth::shim::BtmStatus
bluetooth::shim::Btm::CancelAllReadRemoteDeviceName() {
  if (classic_read_remote_name_.IsInProgress() ||
      le_read_remote_name_.IsInProgress()) {
    if (classic_read_remote_name_.IsInProgress()) {
      hci::Address address;
      hci::Address::FromString(classic_read_remote_name_.AddressString(),
                               address);

      bluetooth::shim::GetName()->CancelRemoteNameRequest(
          address,
          common::BindOnce(
              [](ReadRemoteName* classic_read_remote_name_,
                 hci::ErrorCode status,
                 hci::Address address) { classic_read_remote_name_->Stop(); },
              &classic_read_remote_name_),
          bluetooth::shim::GetGdShimHandler());
    }
    if (le_read_remote_name_.IsInProgress()) {
      LOG_INFO(LOG_TAG, "UNIMPLEMENTED %s need access to GATT module",
               __func__);
    }
    return bluetooth::shim::BTM_UNKNOWN_ADDR;
  }
  LOG_WARN(LOG_TAG,
           "%s Cancelling classic remote device name without one in progress",
           __func__);
  return bluetooth::shim::BTM_WRONG_MODE;
}

void bluetooth::shim::Btm::StartAdvertising() {
  bluetooth::shim::GetAdvertising()->StartAdvertising();
}

void bluetooth::shim::Btm::StopAdvertising() {
  bluetooth::shim::GetAdvertising()->StopAdvertising();
}

void bluetooth::shim::Btm::StartConnectability() {
  bluetooth::shim::GetAdvertising()->StartAdvertising();
}

void bluetooth::shim::Btm::StopConnectability() {
  bluetooth::shim::GetAdvertising()->StopAdvertising();
}

void bluetooth::shim::Btm::StartActiveScanning() {
  StartScanning(kActiveScanning);
}

void bluetooth::shim::Btm::StopActiveScanning() {
  bluetooth::shim::GetScanning()->StopScan(base::Bind([]() {}));
}

void bluetooth::shim::Btm::SetScanningTimer(uint64_t duration_ms,
                                            std::function<void()> func) {
  scanning_timer_->Set(duration_ms, func);
}

void bluetooth::shim::Btm::CancelScanningTimer() { scanning_timer_->Cancel(); }

void bluetooth::shim::Btm::StartObserving() { StartScanning(kPassiveScanning); }

void bluetooth::shim::Btm::StopObserving() { StopActiveScanning(); }

void bluetooth::shim::Btm::SetObservingTimer(uint64_t duration_ms,
                                             std::function<void()> func) {
  observing_timer_->Set(duration_ms, func);
}

void bluetooth::shim::Btm::CancelObservingTimer() {
  observing_timer_->Cancel();
}

namespace bluetooth {
namespace hci {

constexpr int kAdvertisingReportBufferSize = 1024;

struct ExtendedEventTypeOptions {
  bool connectable{false};
  bool scannable{false};
  bool directed{false};
  bool scan_response{false};
  bool legacy{false};
  bool continuing{false};
  bool truncated{false};
};

constexpr uint16_t kBleEventConnectableBit =
    (0x0001 << 0);  // BLE_EVT_CONNECTABLE_BIT
constexpr uint16_t kBleEventScannableBit =
    (0x0001 << 1);  // BLE_EVT_SCANNABLE_BIT
constexpr uint16_t kBleEventDirectedBit =
    (0x0001 << 2);  // BLE_EVT_DIRECTED_BIT
constexpr uint16_t kBleEventScanResponseBit =
    (0x0001 << 3);  // BLE_EVT_SCAN_RESPONSE_BIT
constexpr uint16_t kBleEventLegacyBit = (0x0001 << 4);  // BLE_EVT_LEGACY_BIT
constexpr uint16_t kBleEventIncompleteContinuing = (0x0001 << 5);
constexpr uint16_t kBleEventIncompleteTruncated = (0x0001 << 6);

static void TransformToExtendedEventType(uint16_t* extended_event_type,
                                         ExtendedEventTypeOptions o) {
  ASSERT(extended_event_type != nullptr);
  *extended_event_type = (o.connectable ? kBleEventConnectableBit : 0) |
                         (o.scannable ? kBleEventScannableBit : 0) |
                         (o.directed ? kBleEventDirectedBit : 0) |
                         (o.scan_response ? kBleEventScanResponseBit : 0) |
                         (o.legacy ? kBleEventLegacyBit : 0) |
                         (o.continuing ? kBleEventIncompleteContinuing : 0) |
                         (o.truncated ? kBleEventIncompleteTruncated : 0);
}

class BtmScanningCallbacks : public bluetooth::hci::LeScanningManagerCallbacks {
 public:
  virtual void on_advertisements(
      std::vector<std::shared_ptr<LeReport>> reports) {
    for (auto le_report : reports) {
      uint8_t address_type = static_cast<uint8_t>(le_report->address_type_);
      uint16_t extended_event_type = 0;
      uint8_t* report_data = nullptr;
      size_t report_len = 0;

      uint8_t advertising_data_buffer[kAdvertisingReportBufferSize];
      // Copy gap data, if any, into temporary buffer as payload for legacy
      // stack.
      if (!le_report->gap_data_.empty()) {
        bzero(advertising_data_buffer, kAdvertisingReportBufferSize);
        uint8_t* p = advertising_data_buffer;
        for (auto gap_data : le_report->gap_data_) {
          *p++ = gap_data.data_.size() + sizeof(gap_data.data_type_);
          *p++ = static_cast<uint8_t>(gap_data.data_type_);
          p = (uint8_t*)memcpy(p, &gap_data.data_[0], gap_data.data_.size()) +
              gap_data.data_.size();
        }
        report_data = advertising_data_buffer;
        report_len = p - report_data;
      }

      switch (le_report->GetReportType()) {
        case hci::LeReport::ReportType::ADVERTISING_EVENT: {
          switch (le_report->advertising_event_type_) {
            case hci::AdvertisingEventType::ADV_IND:
              TransformToExtendedEventType(
                  &extended_event_type,
                  {.connectable = true, .scannable = true, .legacy = true});
              break;
            case hci::AdvertisingEventType::ADV_DIRECT_IND:
              TransformToExtendedEventType(
                  &extended_event_type,
                  {.connectable = true, .directed = true, .legacy = true});
              break;
            case hci::AdvertisingEventType::ADV_SCAN_IND:
              TransformToExtendedEventType(&extended_event_type,
                                           {.scannable = true, .legacy = true});
              break;
            case hci::AdvertisingEventType::ADV_NONCONN_IND:
              TransformToExtendedEventType(&extended_event_type,
                                           {.legacy = true});
              break;
            case hci::AdvertisingEventType::
                ADV_DIRECT_IND_LOW:  // SCAN_RESPONSE
              TransformToExtendedEventType(&extended_event_type,
                                           {.connectable = true,
                                            .scannable = true,
                                            .scan_response = true,
                                            .legacy = true});
              break;
            default:
              LOG_WARN(
                  LOG_TAG, "%s Unsupported event type:%s", __func__,
                  AdvertisingEventTypeText(le_report->advertising_event_type_)
                      .c_str());
              return;
          }

          RawAddress raw_address(le_report->address_.address);

          btm_ble_process_adv_addr(raw_address, &address_type);
          btm_ble_process_adv_pkt_cont(
              extended_event_type, address_type, raw_address,
              kPhyConnectionLe1M, kPhyConnectionNone, kAdvDataInfoNotPresent,
              kTxPowerInformationNotPresent, le_report->rssi_,
              kNotPeriodicAdvertisement, report_len, report_data);
        } break;

        case hci::LeReport::ReportType::DIRECTED_ADVERTISING_EVENT:
          LOG_WARN(LOG_TAG,
                   "%s Directed advertising is unsupported from device:%s",
                   __func__, le_report->address_.ToString().c_str());
          break;

        case hci::LeReport::ReportType::EXTENDED_ADVERTISING_EVENT: {
          std::shared_ptr<hci::ExtendedLeReport> extended_le_report =
              std::static_pointer_cast<hci::ExtendedLeReport>(le_report);
          TransformToExtendedEventType(
              &extended_event_type,
              {.connectable = extended_le_report->connectable_,
               .scannable = extended_le_report->scannable_,
               .directed = extended_le_report->directed_,
               .scan_response = extended_le_report->scan_response_,
               .legacy = false,
               .continuing = !extended_le_report->complete_,
               .truncated = extended_le_report->truncated_});
          RawAddress raw_address(le_report->address_.address);
          if (address_type != BLE_ADDR_ANONYMOUS) {
            btm_ble_process_adv_addr(raw_address, &address_type);
          }
          btm_ble_process_adv_pkt_cont(
              extended_event_type, address_type, raw_address,
              kPhyConnectionLe1M, kPhyConnectionNone, kAdvDataInfoNotPresent,
              kTxPowerInformationNotPresent, le_report->rssi_,
              kNotPeriodicAdvertisement, report_len, report_data);

        } break;
      }
    }
  }

  virtual void on_timeout() {
    LOG_WARN(LOG_TAG, "%s Scanning timeout", __func__);
  }
  os::Handler* Handler() { return bluetooth::shim::GetGdShimHandler(); }
};
}  // namespace hci
}  // namespace bluetooth

bluetooth::hci::BtmScanningCallbacks btm_scanning_callbacks;

void bluetooth::shim::Btm::StartScanning(bool use_active_scanning) {
  bluetooth::shim::GetScanning()->StartScan(&btm_scanning_callbacks);
}

size_t bluetooth::shim::Btm::GetNumberOfAdvertisingInstances() const {
  return bluetooth::shim::GetAdvertising()->GetNumberOfAdvertisingInstances();
}

tBTM_STATUS bluetooth::shim::Btm::CreateBond(const RawAddress& bd_addr,
                                             tBLE_ADDR_TYPE addr_type,
                                             tBT_TRANSPORT transport,
                                             uint8_t pin_len, uint8_t* p_pin,
                                             uint32_t trusted_mask[]) {
  auto security_manager =
      bluetooth::shim::GetSecurityModule()->GetSecurityManager();
  switch (transport) {
    case BT_TRANSPORT_BR_EDR:
      security_manager->CreateBond(
          ToAddressWithType(bd_addr.address, BLE_ADDR_PUBLIC));
      break;
    case BT_TRANSPORT_LE:
      security_manager->CreateBondLe(ToAddressWithType(bd_addr, addr_type));
      break;
    default:
      return bluetooth::shim::BTM_ILLEGAL_VALUE;
  }
  return bluetooth::shim::BTM_SUCCESS;
}

bool bluetooth::shim::Btm::CancelBond(const RawAddress& bd_addr) {
  auto security_manager =
      bluetooth::shim::GetSecurityModule()->GetSecurityManager();
  security_manager->CancelBond(ToAddressWithType(bd_addr, BLE_ADDR_PUBLIC));
  return true;
}

bool bluetooth::shim::Btm::RemoveBond(const RawAddress& bd_addr) {
  // TODO(cmanton) Check if acl is connected
  auto security_manager =
      bluetooth::shim::GetSecurityModule()->GetSecurityManager();
  security_manager->RemoveBond(ToAddressWithType(bd_addr, BLE_ADDR_PUBLIC));
  return true;
}

void bluetooth::shim::Btm::SetSimplePairingCallback(
    tBTM_SP_CALLBACK* callback) {
  auto security_manager =
      bluetooth::shim::GetSecurityModule()->GetSecurityManager();
  simple_pairing_callback_ = callback;
}
