#pragma once

#include <algorithm>
#include <cmath>
#include "esphome/core/component.h"
#include "esphome/components/light/light_output.h"
#include "esphome/components/light/light_state.h"
#include "brilliant_rf.h"

namespace esphome {
namespace brilliant_rf {

// One HA light per part of the lamp: the white part (brightness + colour temperature) or the RGB
// ring (colour + brightness). Commands go through BrilliantRF::queue().
class BrilliantLight : public light::LightOutput, public Component {
 public:
  void set_parent(BrilliantRF *parent) { this->parent_ = parent; }
  void set_lamp(uint8_t lamp) { this->lamp_ = lamp; }
  void set_rgb(bool rgb) { this->rgb_ = rgb; }
  void set_mireds(float cold, float warm) {
    this->cold_ = cold;
    this->warm_ = warm;
  }

  light::LightTraits get_traits() override {
    light::LightTraits traits;
    if (this->rgb_) {
      traits.set_supported_color_modes({light::ColorMode::RGB});
    } else {
      traits.set_supported_color_modes({light::ColorMode::COLD_WARM_WHITE});
      traits.set_min_mireds(this->cold_);
      traits.set_max_mireds(this->warm_);
    }
    return traits;
  }

  void write_state(light::LightState *state) override {
    // The first call comes from the restored state at boot: send nothing, otherwise the lamp
    // would switch every time the ESP restarts.
    if (this->first_call_) {
      this->first_call_ = false;
      this->parent_->restore_state(this->lamp_, this->rgb_, state->current_values.is_on());
      return;
    }
    auto &v = state->current_values;
    uint8_t level = (uint8_t) std::max(1L, std::lround(v.get_brightness() * 100));
    if (!v.is_on()) {
      this->parent_->queue(this->lamp_, this->rgb_ ? 1 : 0, this->rgb_ ? CMD_RGB_OFF : CMD_OFF, 0, 0, 0, 0, false);
      return;
    }
    if (this->rgb_) {
      // The lamp wants the hue as shares that add up to 100 (red 100/0/0, yellow 75/25/0) and
      // ignores other sums. p1 is the brightness, so colour and level go out in one command
      // (a separate brightness command would make the ring flash at 100 % in between).
      float r = std::max(0.0f, v.get_red()), g = std::max(0.0f, v.get_green()), b = std::max(0.0f, v.get_blue());
      float sum = r + g + b;
      if (sum <= 0.0f) {
        this->parent_->queue(this->lamp_, 1, CMD_RGB_OFF, 0, 0, 0, 0, false);
        return;
      }
      int rp = (int) std::lround(r / sum * 100), gp = (int) std::lround(g / sum * 100);
      int bp = 100 - rp - gp;
      if (bp < 0) {  // rounding
        gp += bp;
        bp = 0;
      }
      this->parent_->queue(this->lamp_, 1, CMD_RGB, level, rp, gp, bp, true);
    } else {
      float share = (v.get_color_temperature() - this->cold_) / (this->warm_ - this->cold_);
      uint8_t warm = (uint8_t) std::lround(std::min(1.0f, std::max(0.0f, share)) * 100);
      this->parent_->queue(this->lamp_, 0, CMD_WHITE, level, warm, 100 - warm, 0, true);
    }
  }

 protected:
  BrilliantRF *parent_{nullptr};
  uint8_t lamp_{0};
  bool rgb_{false};
  float cold_{153};
  float warm_{370};
  bool first_call_{true};
};

}  // namespace brilliant_rf
}  // namespace esphome
