#pragma once

#include "lift_dpi_button_session.hpp"
#include "logger.hpp"
#include <IOKit/hid/IOHIDDevice.h>
#include <algorithm>
#include <nod/nod.hpp>
#include <pqrs/cf/run_loop_thread.hpp>
#include <pqrs/dispatcher.hpp>
#include <pqrs/osx/chrono.hpp>
#include <pqrs/osx/iokit_hid_device.hpp>
#include <pqrs/osx/iokit_return.hpp>
#include <pqrs/thread_wait.hpp>
#include <vector>

namespace krbn::core_service::daemon::device_grabber_details::lift_dpi_button {
//
// Talks HID++ to a Logitech Lift for Mac that is already opened (seized) by iokit_hid_queue_value_monitor.
//
// Threads:
// - IOHIDDevice calls (input report callback registration, IOHIDDeviceSetReport) run on the shared run loop thread,
//   where the device is scheduled and opened/closed. Requests are therefore ordered before a later IOHIDDeviceClose.
// - Protocol state and signals run on the shared dispatcher thread. Replies are handled asynchronously;
//   the dispatcher never waits for a HID++ reply.
//
// Failure behavior:
// If the device does not reply (3 attempts, 2 seconds each) or rejects a request, the button is left to the firmware
// until the device is grabbed again (reconnect, wake, Karabiner-Core-Service restart or configuration change).
//
class lift_dpi_button final : public pqrs::dispatcher::extra::dispatcher_client {
public:
  //
  // Signals (invoked from the shared dispatcher thread)
  //

  nod::signal<void(bool pressed, absolute_time_point time_stamp)> pressed_changed;

  //
  // Methods
  //

  lift_dpi_button(const lift_dpi_button&) = delete;

  lift_dpi_button(pqrs::not_null_shared_ptr_t<pqrs::cf::run_loop_thread> run_loop_thread,
                  IOHIDDeviceRef device)
      : dispatcher_client(),
        run_loop_thread_(run_loop_thread),
        device_(device) {
    pqrs::osx::iokit_hid_device hid_device(device);
    input_report_buffer_.resize(std::max<size_t>(std::tuple_size_v<report>,
                                                 hid_device.find_max_input_report_size().value_or(64)));

    auto wait = pqrs::make_thread_wait();

    run_loop_thread_->enqueue(^{
      if (device_) {
        IOHIDDeviceRegisterInputReportCallback(*device_,
                                               input_report_buffer_.data(),
                                               input_report_buffer_.size(),
                                               static_input_report_callback,
                                               this);
      }

      wait->notify();
    });

    wait->wait_notice();

    enqueue_to_dispatcher([this] {
      handle_result(session_.start());
    });
  }

  ~lift_dpi_button() override {
    detach_from_dispatcher([this] {
      // Do not emit signals while the owner is being destroyed.
      // A held button is released by the owner (device_ungrabbed or pipeline teardown).
      auto r = session_.stop();
      if (r.request) {
        send(*r.request);
      }
    });

    auto wait = pqrs::make_thread_wait();

    run_loop_thread_->enqueue(^{
      if (device_) {
        IOHIDDeviceRegisterInputReportCallback(*device_,
                                               input_report_buffer_.data(),
                                               input_report_buffer_.size(),
                                               nullptr,
                                               nullptr);
      }

      wait->notify();
    });

    wait->wait_notice();
  }

  // Restore the firmware behavior and report a release if the button is held.
  // This method should be called in the shared dispatcher thread while the device is still opened.
  void stop() {
    handle_result(session_.stop());
  }

private:
  static void static_input_report_callback(void* context,
                                           IOReturn result,
                                           void* sender,
                                           IOHIDReportType type,
                                           uint32_t report_id,
                                           uint8_t* data,
                                           CFIndex length) {
    if (result != kIOReturnSuccess ||
        type != kIOHIDReportTypeInput ||
        report_id != long_report_id ||
        data == nullptr ||
        length != std::tuple_size_v<report>) {
      return;
    }

    auto self = static_cast<lift_dpi_button*>(context);
    if (!self) {
      return;
    }

    report r;
    std::copy(data, data + r.size(), std::begin(r));
    auto time_stamp = pqrs::osx::chrono::mach_absolute_time_point();

    self->enqueue_to_dispatcher([self, r, time_stamp] {
      self->handle_result(self->session_.handle_input_report(r.data(), r.size()),
                          time_stamp);
    });
  }

  void handle_result(const session::result& r,
                     absolute_time_point time_stamp = pqrs::osx::chrono::mach_absolute_time_point()) {
    if (r.request) {
      send(*r.request);

      if (session_.get_state() == session::state::waiting_feature_index ||
          session_.get_state() == session::state::waiting_divert) {
        auto generation = session_.get_generation();
        enqueue_to_dispatcher(
            [this, generation] {
              handle_result(session_.handle_timeout(generation));
            },
            when_now() + std::chrono::seconds(2));
      }
    }

    if (r.pressed) {
      pressed_changed(*r.pressed, time_stamp);
    }
  }

  void send(const report& request) const {
    auto device = device_;
    auto r = request;

    run_loop_thread_->enqueue(^{
      if (device) {
        pqrs::osx::iokit_return kr = IOHIDDeviceSetReport(*device,
                                                          kIOHIDReportTypeOutput,
                                                          r[0],
                                                          r.data(),
                                                          r.size());
        if (!kr) {
          logger::get_logger()->info("Lift DPI button: IOHIDDeviceSetReport failed: {0}", kr.to_string());
        }
      }
    });
  }

  pqrs::not_null_shared_ptr_t<pqrs::cf::run_loop_thread> run_loop_thread_;
  pqrs::cf::cf_ptr<IOHIDDeviceRef> device_;
  std::vector<uint8_t> input_report_buffer_;
  session session_;
};
} // namespace krbn::core_service::daemon::device_grabber_details::lift_dpi_button
