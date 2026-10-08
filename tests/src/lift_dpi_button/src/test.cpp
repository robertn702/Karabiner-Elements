#include "../../../src/core/CoreService/include/core_service/daemon/device_grabber_details/lift_dpi_button_session.hpp"
#include "core_configuration/core_configuration.hpp"
#include "manipulator/manipulator_manager.hpp"
#include <boost/ut.hpp>

namespace {
namespace lift = krbn::core_service::daemon::device_grabber_details::lift_dpi_button;

lift::report make_report(std::initializer_list<uint8_t> bytes) {
  lift::report r{};
  size_t i = 0;
  for (auto b : bytes) {
    r[i++] = b;
  }
  return r;
}

lift::session::result input(lift::session& s, const lift::report& r) {
  return s.handle_input_report(r.data(), r.size());
}

// ROOT.getFeature reply (feature index 0x09)
const auto feature_index_reply = make_report({0x11, 0xff, 0x00, 0x0b, 0x09, 0x00, 0x04});
// setCidReporting reply
const auto divert_reply = make_report({0x11, 0xff, 0x09, 0x3b, 0x00, 0xfd, 0x03, 0x00, 0x00});

lift::report pressed_notification(std::initializer_list<uint16_t> cids) {
  auto r = make_report({0x11, 0xff, 0x09, 0x00});
  size_t i = 4;
  for (auto cid : cids) {
    r[i++] = cid >> 8;
    r[i++] = cid & 0xff;
  }
  return r;
}

void make_diverted(lift::session& s) {
  s.start();
  input(s, feature_index_reply);
  input(s, divert_reply);
}

auto device_id_1 = krbn::device_id(1);
auto button1 = krbn::event_queue::event(krbn::momentary_switch_event(pqrs::hid::usage_page::button,
                                                                     pqrs::hid::usage::button::button_1));
auto button2 = krbn::event_queue::event(krbn::momentary_switch_event(pqrs::hid::usage_page::button,
                                                                     pqrs::hid::usage::button::button_2));
auto button6 = krbn::event_queue::event(krbn::momentary_switch_event(pqrs::hid::usage_page::button,
                                                                     pqrs::hid::usage::button::button_6));

krbn::event_queue::not_null_entries_ptr_t make_button_entries(const krbn::event_queue::event& event,
                                                              bool pressed,
                                                              uint64_t time_stamp,
                                                              pqrs::not_null_shared_ptr_t<krbn::pressed_keys_manager> pressed_keys_manager) {
  auto entries = std::make_shared<std::vector<krbn::event_queue::not_null_const_entry_ptr_t>>();
  entries->push_back(std::make_shared<krbn::event_queue::entry>(device_id_1,
                                                                krbn::event_queue::event_time_stamp(krbn::absolute_time_point(time_stamp)),
                                                                event,
                                                                pressed ? krbn::event_type::key_down : krbn::event_type::key_up,
                                                                krbn::event_integer_value::value_t(pressed ? 1 : 0),
                                                                event,
                                                                krbn::event_queue::state::original));
  return krbn::event_queue::utility::insert_device_keys_and_pointing_buttons_are_released_event(entries,
                                                                                                device_id_1,
                                                                                                pressed_keys_manager);
}

bool contains_released_event(krbn::event_queue::not_null_entries_ptr_t entries) {
  return std::ranges::any_of(*entries, [](auto&& e) {
    return e->get_event().get_type() == krbn::event_queue::event::type::device_keys_and_pointing_buttons_are_released;
  });
}
} // namespace

int main() {
  using namespace boost::ut;
  using namespace boost::ut::literals;

  "target"_test = [] {
    expect(lift::target(pqrs::hid::vendor_id::value_t(0x046d), pqrs::hid::product_id::value_t(0xb031)));
    expect(!lift::target(pqrs::hid::vendor_id::value_t(0x046d), pqrs::hid::product_id::value_t(0xb034)));
    expect(!lift::target(pqrs::hid::vendor_id::value_t(0x05ac), pqrs::hid::product_id::value_t(0xb031)));
  };

  "discovery"_test = [] {
    lift::session s;

    auto r = s.start();
    expect(s.get_state() == lift::session::state::waiting_feature_index);
    expect(r.request == make_report({0x11, 0xff, 0x00, 0x0b, 0x1b, 0x04}));
    expect(!r.pressed);

    // start is not repeated
    expect(!s.start().request);

    // The feature index is discovered, not hard-coded.
    r = input(s, make_report({0x11, 0xff, 0x00, 0x0b, 0x05, 0x00, 0x04}));
    expect(s.get_state() == lift::session::state::waiting_divert);
    expect(r.request == make_report({0x11, 0xff, 0x05, 0x3b, 0x00, 0xfd, 0x03, 0x00, 0x00}));

    r = input(s, make_report({0x11, 0xff, 0x05, 0x3b, 0x00, 0xfd, 0x03, 0x00, 0x00}));
    expect(s.get_state() == lift::session::state::diverted);
    expect(!r.request);
  };

  "feature not supported"_test = [] {
    lift::session s;
    s.start();
    input(s, make_report({0x11, 0xff, 0x00, 0x0b, 0x00}));
    expect(s.get_state() == lift::session::state::failed);
    // Nothing was diverted, so nothing to restore.
    expect(!s.stop().request);
  };

  "error replies"_test = [] {
    // ROOT.getFeature error
    {
      lift::session s;
      s.start();
      // An error for another request is ignored.
      input(s, make_report({0x11, 0xff, 0xff, 0x09, 0x3b, 0x02}));
      expect(s.get_state() == lift::session::state::waiting_feature_index);

      input(s, make_report({0x11, 0xff, 0xff, 0x00, 0x0b, 0x02}));
      expect(s.get_state() == lift::session::state::failed);
    }

    // setCidReporting error (e.g. not divertable): the divert is undone immediately, once.
    {
      lift::session s;
      s.start();
      input(s, feature_index_reply);
      auto r = input(s, make_report({0x11, 0xff, 0xff, 0x09, 0x3b, 0x02}));
      expect(s.get_state() == lift::session::state::failed);
      expect(r.request == make_report({0x11, 0xff, 0x09, 0x3b, 0x00, 0xfd, 0x02, 0x00, 0x00}));
      expect(!s.stop().request);
    }

    // The device replies but does not divert.
    {
      lift::session s;
      s.start();
      input(s, feature_index_reply);
      auto r = input(s, make_report({0x11, 0xff, 0x09, 0x3b, 0x00, 0xfd, 0x00}));
      expect(s.get_state() == lift::session::state::failed);
      expect(r.request == make_report({0x11, 0xff, 0x09, 0x3b, 0x00, 0xfd, 0x02, 0x00, 0x00}));
    }
  };

  "malformed and unrelated reports"_test = [] {
    lift::session s;
    s.start();

    // nullptr, short, long
    expect(!s.handle_input_report(nullptr, 20).request);
    expect(!s.handle_input_report(feature_index_reply.data(), 19).request);
    {
      std::array<uint8_t, 64> long_report{};
      std::copy(std::begin(feature_index_reply), std::end(feature_index_reply), std::begin(long_report));
      expect(!s.handle_input_report(long_report.data(), long_report.size()).request);
    }
    // short HID++ report id, other device index, other software id, other feature, other function
    expect(!input(s, make_report({0x10, 0xff, 0x00, 0x0b, 0x09})).request);
    expect(!input(s, make_report({0x11, 0x01, 0x00, 0x0b, 0x09})).request);
    expect(!input(s, make_report({0x11, 0xff, 0x00, 0x01, 0x09})).request);
    expect(!input(s, make_report({0x11, 0xff, 0x02, 0x0b, 0x09})).request);
    expect(!input(s, make_report({0x11, 0xff, 0x00, 0x1b, 0x09})).request);
    // A mouse report
    expect(!input(s, make_report({0x02, 0x01, 0x00, 0x00})).request);
    // Notifications are ignored before diversion.
    expect(!input(s, pressed_notification({lift::dpi_button_cid})).pressed);
    expect(s.get_state() == lift::session::state::waiting_feature_index);

    input(s, feature_index_reply);
    // Reply for another CID
    input(s, make_report({0x11, 0xff, 0x09, 0x3b, 0x00, 0xc3, 0x03}));
    expect(s.get_state() == lift::session::state::waiting_divert);
    // A late duplicate ROOT.getFeature reply
    expect(!input(s, feature_index_reply).request);
    expect(s.get_state() == lift::session::state::waiting_divert);

    input(s, divert_reply);
    expect(s.get_state() == lift::session::state::diverted);

    // Notifications for another feature index / function / software id
    auto n = pressed_notification({lift::dpi_button_cid});
    n[2] = 0x08;
    expect(!input(s, n).pressed);
    n = pressed_notification({lift::dpi_button_cid});
    n[3] = 0x10;
    expect(!input(s, n).pressed);
    n = pressed_notification({lift::dpi_button_cid});
    n[3] = 0x01;
    expect(!input(s, n).pressed);
    expect(s.get_pressed() == false);
  };

  "timeout and retry"_test = [] {
    lift::session s;
    auto r = s.start();
    auto request = *r.request;
    auto generation = s.get_generation();

    // A stale timeout is ignored.
    expect(!s.handle_timeout(generation - 1).request);

    // Retry twice with the same request.
    r = s.handle_timeout(generation);
    expect(r.request == request);
    expect(s.get_generation() != generation);
    expect(!s.handle_timeout(generation).request);

    r = s.handle_timeout(s.get_generation());
    expect(r.request == request);

    // Give up after max_attempts.
    r = s.handle_timeout(s.get_generation());
    expect(!r.request);
    expect(s.get_state() == lift::session::state::failed);

    // Late replies after failure are ignored.
    expect(!input(s, feature_index_reply).request);
    expect(s.get_state() == lift::session::state::failed);
  };

  "timeout after reply is ignored"_test = [] {
    lift::session s;
    s.start();
    auto generation = s.get_generation();
    input(s, feature_index_reply);
    expect(!s.handle_timeout(generation).request);

    generation = s.get_generation();
    input(s, divert_reply);
    expect(!s.handle_timeout(generation).request);
    expect(s.get_state() == lift::session::state::diverted);
  };

  "divert timeout undoes the divert"_test = [] {
    lift::session s;
    s.start();
    input(s, feature_index_reply);
    for (int i = 0; i < lift::max_attempts - 1; ++i) {
      expect(s.handle_timeout(s.get_generation()).request ==
             make_report({0x11, 0xff, 0x09, 0x3b, 0x00, 0xfd, 0x03, 0x00, 0x00}));
    }
    // The device may have applied the request, so it is undone when giving up.
    auto r = s.handle_timeout(s.get_generation());
    expect(s.get_state() == lift::session::state::failed);
    expect(r.request == make_report({0x11, 0xff, 0x09, 0x3b, 0x00, 0xfd, 0x02, 0x00, 0x00}));
    // A late divert reply after giving up does not re-enable the button.
    expect(!input(s, divert_reply).pressed);
    expect(s.get_state() == lift::session::state::failed);
    expect(!input(s, pressed_notification({lift::dpi_button_cid})).pressed);
    expect(!s.stop().request);
  };

  "press and release deduplication"_test = [] {
    lift::session s;
    make_diverted(s);

    auto r = input(s, pressed_notification({lift::dpi_button_cid}));
    expect(r.pressed == std::optional<bool>(true));
    expect(!r.request);

    // Duplicate press
    expect(!input(s, pressed_notification({lift::dpi_button_cid})).pressed);
    // Another diverted control is pressed too; the DPI button stays pressed.
    expect(!input(s, pressed_notification({0x00c3, lift::dpi_button_cid})).pressed);
    expect(!input(s, pressed_notification({0x0001, 0x0002, 0x0003, lift::dpi_button_cid})).pressed);

    // Release
    r = input(s, pressed_notification({0x00c3}));
    expect(r.pressed == std::optional<bool>(false));
    // Duplicate release
    expect(!input(s, pressed_notification({})).pressed);

    // Repeated presses
    for (int i = 0; i < 3; ++i) {
      expect(input(s, pressed_notification({lift::dpi_button_cid})).pressed == std::optional<bool>(true));
      expect(input(s, pressed_notification({})).pressed == std::optional<bool>(false));
    }
  };

  "stop"_test = [] {
    // While held: release and restore.
    {
      lift::session s;
      make_diverted(s);
      input(s, pressed_notification({lift::dpi_button_cid}));

      auto r = s.stop();
      expect(r.pressed == std::optional<bool>(false));
      expect(r.request == make_report({0x11, 0xff, 0x09, 0x3b, 0x00, 0xfd, 0x02, 0x00, 0x00}));
      expect(s.get_pressed() == false);
      expect(s.get_state() == lift::session::state::stopped);

      // stop is idempotent
      r = s.stop();
      expect(!r.request);
      expect(!r.pressed);

      // Events after stop are ignored.
      expect(!input(s, pressed_notification({lift::dpi_button_cid})).pressed);
      expect(!s.handle_timeout(s.get_generation()).request);
    }

    // While released: restore only.
    {
      lift::session s;
      make_diverted(s);
      auto r = s.stop();
      expect(!r.pressed);
      expect(r.request.has_value());
    }

    // While the divert request is in flight: undo it.
    {
      lift::session s;
      s.start();
      input(s, feature_index_reply);
      auto r = s.stop();
      expect(r.request == make_report({0x11, 0xff, 0x09, 0x3b, 0x00, 0xfd, 0x02, 0x00, 0x00}));
      expect(!r.pressed);
      // A late divert reply is ignored.
      input(s, divert_reply);
      expect(s.get_state() == lift::session::state::stopped);
    }

    // Before discovery: nothing to do.
    {
      lift::session s;
      s.start();
      auto r = s.stop();
      expect(!r.request);
      expect(!r.pressed);
    }

    // Before start
    {
      lift::session s;
      expect(!s.stop().request);
      expect(!s.start().request);
    }
  };

  "event queue entries"_test = [] {
    auto pressed_keys_manager = std::make_shared<krbn::pressed_keys_manager>();

    auto entries = lift::make_event_queue_entries(device_id_1,
                                                  true,
                                                  krbn::absolute_time_point(1000),
                                                  pressed_keys_manager);
    expect(entries->size() == 1);
    expect(entries->front()->get_device_id() == device_id_1);
    expect(entries->front()->get_event() == button6);
    expect(entries->front()->get_event_type() == krbn::event_type::key_down);
    expect(entries->front()->get_state() == krbn::event_queue::state::original);

    // Clicking button1 while the DPI button is held must not report that all buttons are released,
    // otherwise modifiers sent by a button6 mapping would be cleared while it is still held.
    expect(!contains_released_event(make_button_entries(button1, true, 2000, pressed_keys_manager)));
    expect(!contains_released_event(make_button_entries(button1, false, 3000, pressed_keys_manager)));

    entries = lift::make_event_queue_entries(device_id_1,
                                             false,
                                             krbn::absolute_time_point(4000),
                                             pressed_keys_manager);
    expect(entries->size() == 2);
    expect(entries->front()->get_event_type() == krbn::event_type::key_up);
    expect(contains_released_event(entries));
    expect(pressed_keys_manager->empty());
  };

  "button6 to right_control"_test = [] {
    auto core_configuration = std::make_shared<krbn::core_configuration::core_configuration>();
    auto parameters = std::make_shared<krbn::core_configuration::details::complex_modifications_parameters>();
    auto manager = std::make_shared<krbn::manipulator::manipulator_manager>();

    // The example DPI mapping
    manager->push_back_manipulator(nlohmann::json::parse(R"(
      {
        "type": "basic",
        "from": { "pointing_button": "button6", "modifiers": { "optional": ["any"] } },
        "to": [{ "key_code": "right_control" }]
      }
    )"),
                                   parameters);

    auto input_queue = std::make_shared<krbn::event_queue::queue>();
    auto output_queue = std::make_shared<krbn::event_queue::queue>();
    auto pressed_keys_manager = std::make_shared<krbn::pressed_keys_manager>();
    uint64_t now = 0;

    auto push = [&](krbn::event_queue::not_null_entries_ptr_t entries) {
      for (const auto& e : *entries) {
        input_queue->push_back_entry(*e);
      }
      now += 1000;
      while (manager->manipulate(input_queue,
                                 output_queue,
                                 krbn::absolute_time_point(now),
                                 core_configuration)) {
      }
    };

    auto right_control_pressed = [&] {
      return output_queue->get_modifier_flag_manager().is_pressed(krbn::modifier_flag::right_control);
    };

    // Hold DPI: right_control is held.
    push(lift::make_event_queue_entries(device_id_1, true, krbn::absolute_time_point(now), pressed_keys_manager));
    expect(right_control_pressed());

    // Ordinary right click while DPI is held: right_control stays held and button2 passes through.
    push(make_button_entries(button2, true, now, pressed_keys_manager));
    expect(right_control_pressed());
    expect(output_queue->get_pointing_button_manager().is_pressed(button2.get_if<krbn::momentary_switch_event>()->get_usage_pair()));
    push(make_button_entries(button2, false, now, pressed_keys_manager));
    expect(right_control_pressed());

    // Release DPI: right_control is released.
    push(lift::make_event_queue_entries(device_id_1, false, krbn::absolute_time_point(now), pressed_keys_manager));
    expect(!right_control_pressed());

    // Held DPI with device disconnection (device_ungrabbed): no stuck right_control.
    push(lift::make_event_queue_entries(device_id_1, true, krbn::absolute_time_point(now), pressed_keys_manager));
    expect(right_control_pressed());
    {
      auto event = krbn::event_queue::event::make_device_ungrabbed_event();
      input_queue->push_back_entry(krbn::event_queue::entry(device_id_1,
                                                            krbn::event_queue::event_time_stamp(krbn::absolute_time_point(now)),
                                                            event,
                                                            krbn::event_type::single,
                                                            std::nullopt,
                                                            event,
                                                            krbn::event_queue::state::virtual_event));
      while (manager->manipulate(input_queue, output_queue, krbn::absolute_time_point(now), core_configuration)) {
      }
    }
    expect(!right_control_pressed());
  };

  return 0;
}
