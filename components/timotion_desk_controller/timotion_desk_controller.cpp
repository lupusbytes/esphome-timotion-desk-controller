#include "timotion_desk_controller.h"
#include "esphome/core/log.h"
#include "esphome/core/helpers.h"
#include <string>

namespace esphome {
namespace timotion_desk_controller {

static const char *TAG = "timotion_desk_controller";

// Lowest physical desk height (cm) for your model.
// Using 71 so that 71 cm maps to position 0.0 and
// the "close" command (target 0%) can be satisfied.
static const float DESK_MIN_HEIGHT = 71;
static const float DESK_MAX_HEIGHT = 130;

static float transform_height_to_position(float height) {
  // Map absolute height (cm) to a 0.0–1.0 cover position.
  // 0.0 = DESK_MIN_HEIGHT (fully down / closed)
  // 1.0 = DESK_MAX_HEIGHT (fully up / open)
  float pos = (height - DESK_MIN_HEIGHT) / (DESK_MAX_HEIGHT - DESK_MIN_HEIGHT);
  if (pos < 0.0f)
    pos = 0.0f;
  if (pos > 1.0f)
    pos = 1.0f;
  return pos;
}

static float transform_position_to_height(float position) {
  // Inverse of transform_height_to_position.
  if (position < 0.0f)
    position = 0.0f;
  if (position > 1.0f)
    position = 1.0f;
  return DESK_MIN_HEIGHT + position * (DESK_MAX_HEIGHT - DESK_MIN_HEIGHT);
}

void TimotionDeskControllerComponent::loop() {}

void TimotionDeskControllerComponent::setup() {
  ESP_LOGCONFIG(TAG, "Setting up Timotion Desk Controller...");
  this->set_interval("update_desk", 200, [this]() { this->move_desk_(); });
}

void TimotionDeskControllerComponent::dump_config() {
  ESP_LOGCONFIG(TAG, "Timotion Desk Controller:");
  ESP_LOGCONFIG(TAG, "  MAC address        : %s", this->parent()->address_str());
  ESP_LOGCONFIG(TAG, "  Notifications      : %s", this->notify_disable_ ? "disable" : "enable");
  LOG_COVER("  ", "Desk", this);
}

void TimotionDeskControllerComponent::gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                                        esp_ble_gattc_cb_param_t *param) {
  switch (event) {
    case ESP_GATTC_WRITE_CHAR_EVT: {
      if (param->write.status != ESP_GATT_OK) {
        ESP_LOGW(TAG, "Error writing char at handle %d, status=%d", param->write.handle, param->write.status);
      }
      break;
    }

    case ESP_GATTC_OPEN_EVT: {
      if (param->open.status == ESP_GATT_OK) {
        ESP_LOGI(TAG, "[%s] Connected successfully!", this->get_name().c_str());
        break;
      }
      break;
    }

    case ESP_GATTC_DISCONNECT_EVT: {
      ESP_LOGW(TAG, "[%s] Disconnected!", this->get_name().c_str());
      this->status_set_warning();
      break;
    }

    case ESP_GATTC_SEARCH_CMPL_EVT: {
      // Look for output handle
      this->output_handle_ = 0;
      auto chr_output = this->parent()->get_characteristic(this->output_service_uuid_, this->output_char_uuid_);
      if (chr_output == nullptr) {
        this->status_set_warning();
        ESP_LOGW(TAG, "No characteristic found at service %s char %s", this->output_service_uuid_.to_string().c_str(),
                 this->output_char_uuid_.to_string().c_str());
        break;
      }
      this->output_handle_ = chr_output->handle;

      // Register for notification
      auto status_notify = esp_ble_gattc_register_for_notify(this->parent()->get_gattc_if(),
                                                             this->parent()->get_remote_bda(), this->output_handle_);
      if (status_notify) {
        ESP_LOGW(TAG, "esp_ble_gattc_register_for_notify failed, status=%d", status_notify);
      }

      // Look for input handle
      this->input_handle_ = 0;
      auto chr_input = this->parent()->get_characteristic(this->input_service_uuid_, this->input_char_uuid_);
      if (chr_input == nullptr) {
        this->status_set_warning();
        ESP_LOGW(TAG, "No characteristic found at service %s char %s", this->input_service_uuid_.to_string().c_str(),
                 this->input_char_uuid_.to_string().c_str());
        break;
      }
      this->input_handle_ = chr_input->handle;

      // Look for control handle
      this->control_handle_ = 0;
      auto chr_control = this->parent()->get_characteristic(this->control_service_uuid_, this->control_char_uuid_);
      if (chr_control == nullptr) {
        this->status_set_warning();
        ESP_LOGW(TAG, "No characteristic found at service %s char %s", this->control_service_uuid_.to_string().c_str(),
                 this->control_char_uuid_.to_string().c_str());
        break;
      }
      this->control_handle_ = chr_control->handle;
      ESP_LOGCONFIG(TAG, "control handle: %d", this->control_handle_);
      this->set_timeout("desk_init", 5000, [this]() { this->read_value_(this->output_handle_); });

      break;
    }

    case ESP_GATTC_READ_CHAR_EVT: {
      if (param->read.conn_id != this->parent()->get_conn_id())
        break;
      if (param->read.status != ESP_GATT_OK) {
        ESP_LOGW(TAG, "Error reading char at handle %d, status=%d", param->read.handle, param->read.status);
        break;
      }
      if (param->read.handle == this->output_handle_) {
        this->status_clear_warning();
        this->publish_cover_state_(param->read.value, param->read.value_len);
      }
      break;
    }

    case ESP_GATTC_NOTIFY_EVT: {
      if (param->notify.conn_id != this->parent()->get_conn_id() || param->notify.handle != this->output_handle_)
        break;
      ESP_LOGV(TAG, "[%s] ESP_GATTC_NOTIFY_EVT: handle=0x%x, value=0x%x", this->get_name().c_str(),
               param->notify.handle, param->notify.value[0]);
      this->publish_cover_state_(param->notify.value, param->notify.value_len);
      break;
    }

    case ESP_GATTC_REG_FOR_NOTIFY_EVT: {
      this->node_state = espbt::ClientState::ESTABLISHED;
      if (param->reg_for_notify.status == ESP_GATT_OK) {
        this->notify_disable_ = false;
      }
      break;
    }

    default:
      break;
  }
}

void TimotionDeskControllerComponent::write_value_(uint16_t handle, const uint8_t *data, uint16_t len) {
  ESP_LOGD(">>>> ", "write_value_");
  ESP_LOGD(TAG, "handle: %d, len: %u", handle, len);

  esp_err_t status = ::esp_ble_gattc_write_char(this->parent()->get_gattc_if(), this->parent()->get_conn_id(), handle,
                                                len, const_cast<uint8_t *>(data), ESP_GATT_WRITE_TYPE_NO_RSP,
                                                ESP_GATT_AUTH_REQ_NONE);

  if (status != ESP_OK) {
    this->status_set_warning();
    ESP_LOGW(TAG, "[%s] Error sending write request for cover, status=%d", this->get_name().c_str(), status);
  }
}

void TimotionDeskControllerComponent::read_value_(uint16_t handle) {
  auto status_read = esp_ble_gattc_read_char(this->parent()->get_gattc_if(), this->parent()->get_conn_id(), handle,
                                             ESP_GATT_AUTH_REQ_NONE);
  if (status_read) {
    this->status_set_warning();
    ESP_LOGW(TAG, "[%s] Error sending read request for cover, status=%d", this->get_name().c_str(), status_read);
  }
}

cover::CoverTraits TimotionDeskControllerComponent::get_traits() {
  auto traits = cover::CoverTraits();
  traits.set_is_assumed_state(false);
  traits.set_supports_position(true);
  traits.set_supports_tilt(false);
  return traits;
}

void TimotionDeskControllerComponent::publish_cover_state_(uint8_t *value, uint16_t value_len) {
  // Timotion "simple" notification format as observed from the working
  // ble_client sensors in the example YAML:
  //   x[1] : motion/state (65 = down, 66 = up, otherwise idle)
  //   x[3] : current height (cm)
  if (value_len < 4) {
    return;
  }

  std::vector<uint8_t> x(value, value + value_len);

  uint16_t speed_raw = x[1];
  uint16_t height = x[3];

  if (this->lastHeight == height && this->lastSpeed == speed_raw)
    return;

  this->lastHeight = height;
  this->lastSpeed = speed_raw;

  float position = transform_height_to_position(static_cast<float>(height));
  ESP_LOGCONFIG(TAG, "publish speed=%d height=%d pos=%f target=%f", speed_raw, height, position,
                this->position_target_);

  if (speed_raw == 65) {
    // Moving down
    this->current_operation = cover::COVER_OPERATION_CLOSING;
  } else if (speed_raw == 66) {
    // Moving up
    this->current_operation = cover::COVER_OPERATION_OPENING;
  } else {
    // Idle or unknown
    this->current_operation = cover::COVER_OPERATION_IDLE;
  }

  this->position = position;
  this->publish_state(false);
}

void TimotionDeskControllerComponent::move_desk_() {
  if (this->notify_disable_) {
    if (this->controlled_ || this->current_operation != cover::COVER_OPERATION_IDLE) {
      this->read_value_(this->output_handle_);
    }
  }

  if (!this->controlled_) {
    return;
  }

  // Check if target has been reached
  if (this->is_at_target_()) {
    ESP_LOGD(TAG, "Update Desk - target reached");
    this->stop_move_();
    return;
  }

  if (this->notify_disable_) {
    if (this->current_operation == cover::COVER_OPERATION_IDLE) {
      this->not_moving_loop_++;
      if (this->not_moving_loop_ > 4) {
        ESP_LOGD(TAG, "Update Desk - desk not moving");
        this->stop_move_();
      }
    } else {
      this->not_moving_loop_ = 0;
    }
  }

  ESP_LOGD(TAG, "Update Desk - Move from %.0f to %.0f", this->position * 100, this->position_target_ * 100);
  this->move_torwards_();
}

void TimotionDeskControllerComponent::control(const cover::CoverCall &call) {
  if (this->notify_disable_) {
    this->read_value_(this->output_handle_);
  }

  if (call.get_position().has_value()) {
    if (this->current_operation != cover::COVER_OPERATION_IDLE) {
      this->stop_move_();
    }

    this->position_target_ = *call.get_position();

    if (this->position == this->position_target_) {
      return;
    }

    if (this->position_target_ > this->position) {
      this->current_operation = cover::COVER_OPERATION_OPENING;
    } else {
      this->current_operation = cover::COVER_OPERATION_CLOSING;
    }

    this->start_move_torwards_();
    return;
  }

  if (call.get_stop()) {
    ESP_LOGD(TAG, "Cover control - STOP");
    this->stop_move_();
  }
}

void TimotionDeskControllerComponent::start_move_torwards_() {
  this->controlled_ = true;
  if (this->notify_disable_) {
    this->not_moving_loop_ = 0;
  }
  //   if (false == this->use_only_up_down_command_) {
  //     this->write_value_(this->control_handle_, 0xFE);
  //     this->write_value_(this->control_handle_, 0xFF);
  //   }
}

void TimotionDeskControllerComponent::move_torwards_() {
  // Use the module-style commands captured from the mobile app.
  // For now, send fixed UP/DOWN frames as observed from the app.
  static const uint8_t CMD_UP[] = {
      0x02, 0x03, 0x00, 0x0c, 0x00, 0x08, 0x00, 0x04, 0x00,
      0x52, 0x0e, 0x00, 0xd9, 0xff, 0x01, 0x60, 0x39};
  static const uint8_t CMD_DOWN[] = {
      0x02, 0x03, 0x00, 0x0c, 0x00, 0x08, 0x00, 0x04, 0x00,
      0x52, 0x0e, 0x00, 0xd9, 0xff, 0x02, 0x60, 0x3a};

  if (this->current_operation == cover::COVER_OPERATION_OPENING) {
    this->write_value_(this->control_handle_, CMD_UP, sizeof(CMD_UP));
  } else if (this->current_operation == cover::COVER_OPERATION_CLOSING) {
    this->write_value_(this->control_handle_, CMD_DOWN, sizeof(CMD_DOWN));
  }
}

void TimotionDeskControllerComponent::stop_move_() {
  // No explicit STOP command captured; rely on idle/target logic.
  //   if (false == this->use_only_up_down_command_) {
  //     this->write_value_(this->input_handle_, 0x8001);
  //   }

  this->current_operation = cover::COVER_OPERATION_IDLE;
  this->controlled_ = false;
}

bool TimotionDeskControllerComponent::is_at_target_() const {
  switch (this->current_operation) {
    case cover::COVER_OPERATION_OPENING:
      return this->position >= this->position_target_;
    case cover::COVER_OPERATION_CLOSING:
      return this->position <= this->position_target_;
    case cover::COVER_OPERATION_IDLE:
      if (this->notify_disable_) {
        return !this->controlled_;
      }
    //   return !this->controlled_;
    default:
      return true;
  }
}

espbt::ESPBTUUID uuid128_from_string(std::string value) {
  esp_bt_uuid_t m_uuid;
  m_uuid.len = ESP_UUID_LEN_128;
  int n = 0;
  for (int i = 0; i < value.length();) {
    if (value.c_str()[i] == '-') {
      i++;
    }
    uint8_t MSB = value.c_str()[i];
    uint8_t LSB = value.c_str()[i + 1];
    if (MSB > '9') {
      MSB -= 7;
    }
    if (LSB > '9') {
      LSB -= 7;
    }
    m_uuid.uuid.uuid128[15 - n++] = ((MSB & 0x0F) << 4) | (LSB & 0x0F);
    i += 2;
  }

  return espbt::ESPBTUUID::from_uuid(m_uuid);
}

}  // namespace timotion_desk_controller
}  // namespace esphome
