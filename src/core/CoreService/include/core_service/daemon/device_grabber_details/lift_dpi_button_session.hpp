#pragma once

#include "event_queue.hpp"
#include "logger.hpp"
#include "pressed_keys_manager.hpp"
#include "types.hpp"
#include <array>
#include <cstdint>
#include <optional>

namespace krbn::core_service::daemon::device_grabber_details::lift_dpi_button {
//
// HID++ 2.0 protocol state for the Logitech Lift for Mac DPI (cursor speed) button.
//
// The button is temporarily diverted with REPROG_CONTROLS_V4 so that the device reports it via HID++ notifications
// instead of changing the cursor speed, and Karabiner-Core-Service reports it as pointing_button `button6`.
// This class contains no IOKit calls so that it can be tested without hardware.
// All methods must be called from the same thread (the shared dispatcher thread).
//

using report = std::array<uint8_t, 20>;

constexpr uint8_t long_report_id = 0x11;
constexpr uint8_t ble_device_index = 0xff;
constexpr uint8_t error_feature_index = 0xff;
constexpr uint8_t software_id = 0x0b;
constexpr uint16_t reprog_controls_v4_feature_id = 0x1b04;
constexpr uint16_t dpi_button_cid = 0x00fd;
constexpr int max_attempts = 3;

// REPROG_CONTROLS_V4 setCidReporting flags: divert (bit 0) and its valid bit (bit 1).
constexpr uint8_t divert_flags = 0x03;
constexpr uint8_t undivert_flags = 0x02;

inline bool target(pqrs::hid::vendor_id::value_t vendor_id,
                   pqrs::hid::product_id::value_t product_id) {
  // Logitech Lift for Mac (Bluetooth Low Energy)
  return vendor_id == pqrs::hid::vendor_id::value_t(0x046d) &&
         product_id == pqrs::hid::product_id::value_t(0xb031);
}

class session final {
public:
  enum class state {
    initial,
    waiting_feature_index,
    waiting_divert,
    diverted,
    failed,
    stopped,
  };

  struct result final {
    // A HID++ output report to send.
    std::optional<report> request;
    // A DPI button state change to report.
    std::optional<bool> pressed;
  };

  [[nodiscard]] state get_state() const {
    return state_;
  }

  [[nodiscard]] bool get_pressed() const {
    return pressed_;
  }

  [[nodiscard]] uint64_t get_generation() const {
    return generation_;
  }

  // ROOT.getFeature(REPROG_CONTROLS_V4)
  result start() {
    if (state_ != state::initial) {
      return {};
    }

    return send(state::waiting_feature_index,
                make_request(0x00,
                             0,
                             {static_cast<uint8_t>(reprog_controls_v4_feature_id >> 8),
                              static_cast<uint8_t>(reprog_controls_v4_feature_id & 0xff)}));
  }

  result handle_input_report(const uint8_t* data, size_t size) {
    if (data == nullptr ||
        size != std::tuple_size_v<report> ||
        data[0] != long_report_id ||
        data[1] != ble_device_index) {
      return {};
    }

    // HID++ 2.0 error: [report_id, device_index, 0xff, feature_index, function|software_id, error_code, ...]
    if (data[2] == error_feature_index) {
      if (pending_request_ &&
          data[3] == (*pending_request_)[2] &&
          data[4] == (*pending_request_)[3]) {
        logger::get_logger()->warn("Lift DPI button: HID++ error 0x{0:02x} for feature index 0x{1:02x} function 0x{2:02x}",
                                   data[5],
                                   data[3],
                                   data[4] >> 4);
        return fail();
      }
      return {};
    }

    uint8_t function = data[3] >> 4;
    uint8_t sw_id = data[3] & 0x0f;

    // Notifications always use software id 0.
    if (sw_id == 0) {
      return handle_notification(data, function);
    }

    if (sw_id != software_id || !pending_request_) {
      return {};
    }

    switch (state_) {
      case state::waiting_feature_index: {
        if (data[2] != 0x00 || function != 0) {
          return {};
        }

        // ROOT.getFeature reply: [feature_index, feature_type, feature_version]
        auto index = data[4];
        if (index == 0x00 || index == error_feature_index) {
          logger::get_logger()->warn("Lift DPI button: REPROG_CONTROLS_V4 is not supported");
          return fail();
        }

        feature_index_ = index;
        divert_requested_ = true;
        return send(state::waiting_divert,
                    make_request(index,
                                 3,
                                 {static_cast<uint8_t>(dpi_button_cid >> 8),
                                  static_cast<uint8_t>(dpi_button_cid & 0xff),
                                  divert_flags,
                                  0x00,
                                  0x00}));
      }

      case state::waiting_divert: {
        if (data[2] != feature_index_ || function != 3) {
          return {};
        }

        // setCidReporting reply echoes [cid, flags, remap].
        uint16_t cid = (static_cast<uint16_t>(data[4]) << 8) | data[5];
        if (cid != dpi_button_cid) {
          return {};
        }

        pending_request_ = std::nullopt;

        if ((data[6] & 0x01) == 0) {
          logger::get_logger()->warn("Lift DPI button: the device did not divert the DPI button");
          return fail();
        }

        state_ = state::diverted;
        logger::get_logger()->info("Lift DPI button: diverted (REPROG_CONTROLS_V4 feature index 0x{0:02x})",
                                   feature_index_);
        return {};
      }

      case state::initial:
      case state::diverted:
      case state::failed:
      case state::stopped:
        return {};
    }

    return {};
  }

  result handle_timeout(uint64_t generation) {
    if (generation != generation_ ||
        !pending_request_ ||
        (state_ != state::waiting_feature_index && state_ != state::waiting_divert)) {
      return {};
    }

    if (attempts_ >= max_attempts) {
      logger::get_logger()->warn("Lift DPI button: no HID++ reply after {0} attempts", attempts_);
      return fail();
    }

    ++attempts_;
    ++generation_;
    return result{.request = pending_request_};
  }

  // Returns the request that restores the firmware behavior if the button might be diverted,
  // and a release if the button is pressed.
  result stop() {
    result r;

    if (state_ == state::stopped) {
      return r;
    }

    if (divert_requested_) {
      divert_requested_ = false;
      r.request = make_undivert_request();
    }

    if (pressed_) {
      pressed_ = false;
      r.pressed = false;
    }

    state_ = state::stopped;
    pending_request_ = std::nullopt;
    ++generation_;

    return r;
  }

private:
  static report make_request(uint8_t feature_index,
                             uint8_t function,
                             std::initializer_list<uint8_t> parameters) {
    report r{};
    r[0] = long_report_id;
    r[1] = ble_device_index;
    r[2] = feature_index;
    r[3] = static_cast<uint8_t>((function << 4) | software_id);

    size_t i = 4;
    for (auto p : parameters) {
      r[i++] = p;
    }

    return r;
  }

  result send(state new_state, const report& request) {
    state_ = new_state;
    pending_request_ = request;
    attempts_ = 1;
    ++generation_;
    return result{.request = request};
  }

  result handle_notification(const uint8_t* data, uint8_t function) {
    // REPROG_CONTROLS_V4 divertedButtonsEvent: up to four pressed CIDs (big endian).
    if (state_ != state::diverted ||
        data[2] != feature_index_ ||
        function != 0) {
      return {};
    }

    bool pressed = false;
    for (size_t i = 4; i <= 10; i += 2) {
      if (((static_cast<uint16_t>(data[i]) << 8) | data[i + 1]) == dpi_button_cid) {
        pressed = true;
      }
    }

    if (pressed == pressed_) {
      return {};
    }

    pressed_ = pressed;
    return result{.pressed = pressed};
  }

  // Undo a divert request that the device might have applied, so the button keeps working as the firmware button.
  result fail() {
    state_ = state::failed;
    pending_request_ = std::nullopt;

    result r;
    if (divert_requested_) {
      divert_requested_ = false;
      r.request = make_undivert_request();
    }
    return r;
  }

  report make_undivert_request() const {
    return make_request(feature_index_,
                        3,
                        {static_cast<uint8_t>(dpi_button_cid >> 8),
                         static_cast<uint8_t>(dpi_button_cid & 0xff),
                         undivert_flags,
                         0x00,
                         0x00});
  }

  state state_ = state::initial;
  uint8_t feature_index_ = 0;
  bool divert_requested_ = false;
  bool pressed_ = false;
  std::optional<report> pending_request_;
  int attempts_ = 0;
  uint64_t generation_ = 0;
};

// Make button6 entries for a DPI button state change, tracking it in the device's pressed_keys_manager
// like a physical button so that `device_keys_and_pointing_buttons_are_released` is not sent while it is held.
inline event_queue::not_null_entries_ptr_t make_event_queue_entries(device_id device_id,
                                                                    bool pressed,
                                                                    absolute_time_point time_stamp,
                                                                    pqrs::not_null_shared_ptr_t<pressed_keys_manager> pressed_keys_manager) {
  event_queue::event event(momentary_switch_event(pqrs::hid::usage_page::button,
                                                  pqrs::hid::usage::button::button_6));

  auto entries = std::make_shared<std::vector<event_queue::not_null_const_entry_ptr_t>>();
  entries->push_back(std::make_shared<event_queue::entry>(device_id,
                                                          event_queue::event_time_stamp(time_stamp),
                                                          event,
                                                          pressed ? event_type::key_down : event_type::key_up,
                                                          event_integer_value::value_t(pressed ? 1 : 0),
                                                          event,
                                                          event_queue::state::original));

  return event_queue::utility::insert_device_keys_and_pointing_buttons_are_released_event(entries,
                                                                                          device_id,
                                                                                          pressed_keys_manager);
}
} // namespace krbn::core_service::daemon::device_grabber_details::lift_dpi_button
