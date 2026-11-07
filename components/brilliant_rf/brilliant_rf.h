#pragma once

#include <vector>
#include "esphome/core/component.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/preferences.h"
#include "esphome/components/spi/spi.h"

namespace esphome {
namespace brilliant_rf {

// Command codes of the 98770/05 remote (docs/PROTOCOL.md has the full table)
static const uint8_t CMD_ON = 0x01;
static const uint8_t CMD_OFF = 0x09;
static const uint8_t CMD_WHITE = 0x12;          // p1 brightness %, p2 warm %, p3 cold % (warm + cold = 100)
static const uint8_t CMD_RGB = 0x11;            // p1 brightness %, p2..p4 red/green/blue % (sum = 100)
static const uint8_t CMD_RGB_BRIGHTNESS = 0x13; // p1 brightness %
static const uint8_t CMD_RGB_OFF = 0x1B;

class BrilliantRF : public Component,
                    public spi::SPIDevice<spi::BIT_ORDER_MSB_FIRST, spi::CLOCK_POLARITY_LOW,
                                          spi::CLOCK_PHASE_LEADING, spi::DATA_RATE_4MHZ> {
 public:
  void set_ce_pin(GPIOPin *pin) { this->ce_pin_ = pin; }
  void set_channel(uint8_t channel) { this->channel_ = channel; }
  void set_sync_word(const std::vector<uint8_t> &sync) {
    for (size_t i = 0; i < 4 && i < sync.size(); i++)
      this->sync_[i] = sync[i];
  }
  void set_repeats(uint8_t repeats) { this->repeats_ = repeats; }
  void set_log_packets(bool log) { this->log_packets_ = log; }
  void set_scan(uint8_t first, uint8_t last, uint32_t dwell_ms) {
    this->scan_ = true;
    this->scan_first_ = first;
    this->scan_last_ = last;
    this->scan_dwell_ms_ = dwell_ms;
  }
  void add_lamp(const std::vector<uint8_t> &remote_id);

  // Send a command to a lamp; rolling counter and byte 13 are handled automatically
  void send(uint8_t lamp, uint8_t command, uint8_t p1 = 0, uint8_t p2 = 0, uint8_t p3 = 0, uint8_t p4 = 0);
  // Debugging: send with an explicit counter and byte 13
  void send_raw(uint8_t lamp, uint8_t command, uint8_t p1, uint8_t p2, uint8_t p3, uint8_t p4, uint8_t counter,
                uint8_t byte13);
  // Debugging: set the counter the lamp last accepted (the next packet carries value + 1)
  void set_counter(uint8_t lamp, uint8_t value);

  // Used by the light platform. Slot 0 = white, slot 1 = RGB ring. The loop sends at most every
  // 150 ms, so dragging a slider does not turn into dozens of radio bursts.
  void queue(uint8_t lamp, uint8_t slot, uint8_t command, uint8_t p1, uint8_t p2, uint8_t p3, uint8_t p4,
             bool power_on);
  // Used by the light platform after boot: remember the restored state without sending
  void restore_state(uint8_t lamp, bool rgb, bool on) {
    if (lamp < this->lamps_.size())
      (rgb ? this->lamps_[lamp].rgb_on : this->lamps_[lamp].white_on) = on;
  }

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::DATA; }

 protected:
  static const uint8_t PAYLOAD = 32;

  struct Pending {
    bool open;
    uint8_t command;
    uint8_t p[4];
    bool power_on;
  };
  // Per-lamp state. The lamp accepts a packet only if its counter was not used recently; it
  // allows big jumps forward but only ~40 steps back, and it does not wrap from FF to 00.
  struct Lamp {
    uint8_t remote_id[5];
    uint8_t counter{0x80};         // last counter we used or saw the lamp accept
    uint8_t remote_counter{0x80};  // last counter heard from the original remote
    uint8_t byte13{0x01};          // learned from the remote (colour commands always use 02)
    ESPPreferenceObject counter_pref, remote_counter_pref, byte13_pref;
    bool lamp_on{false};  // unknown after boot: rather send one ON too many
    bool white_on{false};
    bool rgb_on{false};
    uint8_t white_warm{50};
    uint8_t white_level{100};
    Pending pending[2]{};
  };

  uint8_t read_reg_(uint8_t reg);
  void write_reg_(uint8_t reg, uint8_t value);
  void write_reg_bytes_(uint8_t reg, const uint8_t *data, size_t len);
  void command_(uint8_t cmd);
  bool probe_();
  void configure_rx_();
  void set_rx_channel_(uint8_t channel);
  void receive_();
  void handle_frame_(const uint8_t *payload, uint8_t channel);

  bool select_(uint8_t lamp);
  uint8_t next_counter_();
  void store_counter_(uint8_t value);
  void lower_counter_();
  void send_selected_(uint8_t command, uint8_t p1 = 0, uint8_t p2 = 0, uint8_t p3 = 0, uint8_t p4 = 0);
  void transmit_(uint8_t counter, uint8_t command, uint8_t p1, uint8_t p2, uint8_t p3, uint8_t p4);

  GPIOPin *ce_pin_{nullptr};
  uint8_t channel_{50};
  uint8_t sync_[4]{0x8C, 0x42, 0xF3, 0x7B};
  uint8_t repeats_{40};
  bool log_packets_{false};
  bool scan_{false};
  uint8_t scan_first_{0};
  uint8_t scan_last_{83};
  uint32_t scan_dwell_ms_{2};

  std::vector<Lamp> lamps_;
  Lamp *active_{nullptr};
  int byte13_override_{-1};

  bool found_{false};
  uint32_t last_probe_{0};
  uint8_t rx_channel_{0};
  uint32_t rx_channel_since_{0};
  uint32_t last_send_{0};

  HighFrequencyLoopRequester high_freq_;
};

}  // namespace brilliant_rf
}  // namespace esphome
