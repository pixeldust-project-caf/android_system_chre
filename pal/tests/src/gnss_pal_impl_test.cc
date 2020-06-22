/*
 * Copyright (C) 2020 The Android Open Source Project
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

#include "gnss_pal_impl_test.h"

#include "chre/platform/log.h"
#include "chre/platform/shared/pal_system_api.h"
#include "chre/platform/system_time.h"
#include "chre/util/lock_guard.h"

#include <cinttypes>

// Flag to require GNSS location sessions capability to be enabled for the test
// to pass. Set to false to allow tests to pass on disabled platforms.
// Note that it is required to run this test where location can be acquired.
// The constants kGnssLocationEventTimeoutNs and kLocationEventArraySize
// may be tuned if applicable.
#ifndef PAL_IMPL_TEST_GNSS_LOCATION_REQUIRED
#define PAL_IMPL_TEST_GNSS_LOCATION_REQUIRED true
#endif

namespace gnss_pal_impl_test {

namespace {

using ::chre::Nanoseconds;
using ::chre::Seconds;
using ::chre::SystemTime;

//! A pointer to the current test running
gnss_pal_impl_test::PalGnssTest *gTest = nullptr;

//! Timeout as specified by the CHRE API
const Nanoseconds kGnssAsyncResultTimeoutNs =
    Nanoseconds(CHRE_GNSS_ASYNC_RESULT_TIMEOUT_NS);

//! Timeout to wait for kLocationEventArraySize location events.
const Nanoseconds kGnssLocationEventTimeoutNs = Seconds(60);

void chrePalRequestStateResync() {
  if (gTest != nullptr) {
    gTest->requestStateResync();
  }
}

void chrePalLocationStatusChangeCallback(bool enabled, uint8_t errorCode) {
  if (gTest != nullptr) {
    gTest->locationStatusChangeCallback(enabled, errorCode);
  }
}

void chrePalLocationEventCallback(struct chreGnssLocationEvent *event) {
  if (gTest != nullptr) {
    gTest->locationEventCallback(event);
  }
}

void chrePalMeasurementStatusChangeCallback(bool enabled, uint8_t errorCode) {
  if (gTest != nullptr) {
    gTest->measurementStatusChangeCallback(enabled, errorCode);
  }
}

void chrePalMeasurementEventCallback(struct chreGnssDataEvent *event) {
  if (gTest != nullptr) {
    gTest->measurementEventCallback(event);
  }
}

void logLocationEvent(const struct chreGnssLocationEvent &event) {
  LOGI("Received location: %" PRId32 ", %" PRId32, event.latitude_deg_e7,
       event.longitude_deg_e7);
  LOGI("  timestamp (ms): %" PRIu64, event.timestamp);
  LOGI("  altitude (m): %f", event.altitude);
  LOGI("  speed (m/s): %f", event.speed);
  LOGI("  bearing (deg): %f", event.bearing);
  LOGI("  accuracy: %f", event.accuracy);
  LOGI("  flags: 0x%" PRIx16, event.flags);
  LOGI("  altitude_accuracy: %f", event.altitude_accuracy);
  LOGI("  speed_accuracy: %f", event.speed_accuracy);
  LOGI("  bearing_accuracy: %f", event.bearing_accuracy);
}

void validateLocationEvent(const struct chreGnssLocationEvent &event) {
  static uint64_t sLastTimestampNs = 0;
  EXPECT_GE(event.timestamp, sLastTimestampNs);
  sLastTimestampNs = event.timestamp;
  if (event.flags & CHRE_GPS_LOCATION_HAS_LAT_LONG) {
    EXPECT_GE(event.latitude_deg_e7, -90 * 1e7);
    EXPECT_LE(event.latitude_deg_e7, 90 * 1e7);
    EXPECT_GE(event.longitude_deg_e7, -180 * 1e7);
    EXPECT_LE(event.longitude_deg_e7, 180 * 1e7);
  }
  if (event.flags & CHRE_GPS_LOCATION_HAS_BEARING) {
    EXPECT_GE(event.bearing, 0);
    EXPECT_LT(event.bearing, 360);  // [0, 360) per API
  }
}

}  // anonymous namespace

void PalGnssTest::SetUp() {
  api_ = chrePalGnssGetApi(CHRE_PAL_GNSS_API_CURRENT_VERSION);
  ASSERT_NE(api_, nullptr);
  EXPECT_EQ(api_->moduleVersion, CHRE_PAL_GNSS_API_CURRENT_VERSION);

  // Open the PAL API
  static const struct chrePalGnssCallbacks kCallbacks = {
      .requestStateResync = chrePalRequestStateResync,
      .locationStatusChangeCallback = chrePalLocationStatusChangeCallback,
      .locationEventCallback = chrePalLocationEventCallback,
      .measurementStatusChangeCallback = chrePalMeasurementStatusChangeCallback,
      .measurementEventCallback = chrePalMeasurementEventCallback,
  };
  ASSERT_TRUE(api_->open(&chre::gChrePalSystemApi, &kCallbacks));
  gTest = this;

  errorCode_ = CHRE_ERROR_LAST;
  locationSessionEnabled_ = false;
  locationEventVector_.resize(0);
}

void PalGnssTest::TearDown() {
  gTest = nullptr;
  api_->close();
}

void PalGnssTest::requestStateResync() {
  // TODO:
}

void PalGnssTest::locationStatusChangeCallback(bool enabled,
                                               uint8_t errorCode) {
  LOGI("Received location status change with enabled %d error %" PRIu8, enabled,
       errorCode);
  if (errorCode == CHRE_ERROR_LAST) {
    LOGE("Received CHRE_ERROR_LAST");
    errorCode = CHRE_ERROR;
  }
  chre::LockGuard<chre::Mutex> lock(mutex_);
  errorCode_ = errorCode;
  locationSessionEnabled_ = enabled;
  condVar_.notify_one();
}

void PalGnssTest::locationEventCallback(struct chreGnssLocationEvent *event) {
  LOGI("Received location event");
  chre::LockGuard<chre::Mutex> lock(mutex_);
  if (!locationEventVector_.full()) {
    locationEventVector_.push_back(event);
    if (locationEventVector_.full()) {
      condVar_.notify_one();
    }
  }
}

void PalGnssTest::measurementStatusChangeCallback(bool enabled,
                                                  uint8_t errorCode) {
  // TODO:
}

void PalGnssTest::measurementEventCallback(struct chreGnssDataEvent *event) {
  // TODO:
}

void PalGnssTest::waitForAsyncResponseAssertSuccess(
    chre::Nanoseconds timeoutNs) {
  bool waitSuccess = true;
  while (errorCode_ == CHRE_ERROR_LAST && waitSuccess) {
    waitSuccess = condVar_.wait_for(mutex_, timeoutNs);
  }
  ASSERT_TRUE(waitSuccess);
  ASSERT_EQ(errorCode_, CHRE_ERROR_NONE);
}

TEST_F(PalGnssTest, LocationSessionTest) {
  bool hasLocationCapability =
      ((api_->getCapabilities() & CHRE_GNSS_CAPABILITIES_LOCATION) ==
       CHRE_GNSS_CAPABILITIES_LOCATION);
#if PAL_IMPL_TEST_GNSS_LOCATION_REQUIRED
  ASSERT_TRUE(hasLocationCapability);
#else
  if (!hasLocationCapability) {
    GTEST_SKIP();
  }
#endif

  chre::LockGuard<chre::Mutex> lock(mutex_);

  prepareForAsyncResponse();
  ASSERT_TRUE(api_->controlLocationSession(
      true /* enable */, 1000 /* minIntervalMs */, 0 /* minTimeToNextFixMs */));
  waitForAsyncResponseAssertSuccess(kGnssAsyncResultTimeoutNs);
  ASSERT_TRUE(locationSessionEnabled_);

  bool waitSuccess = true;
  while (!locationEventVector_.full() && waitSuccess) {
    waitSuccess = condVar_.wait_for(mutex_, kGnssLocationEventTimeoutNs);
  }

  for (size_t i = 0; i < locationEventVector_.size(); i++) {
    logLocationEvent(*locationEventVector_[i]);
    validateLocationEvent(*locationEventVector_[i]);
    api_->releaseLocationEvent(locationEventVector_[i]);
  }

  prepareForAsyncResponse();
  ASSERT_TRUE(api_->controlLocationSession(
      false /* enable */, 0 /* minIntervalMs */, 0 /* minTimeToNextFixMs */));
  waitForAsyncResponseAssertSuccess(kGnssAsyncResultTimeoutNs);
  ASSERT_FALSE(locationSessionEnabled_);
}

}  // namespace gnss_pal_impl_test
