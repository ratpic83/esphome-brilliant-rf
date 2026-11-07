#include "brilliant_rf.h"
#include "esphome/core/log.h"
#include <algorithm>
#include <cstring>

namespace esphome {
namespace brilliant_rf {

static const char *const TAG = "brilliant_rf";

// nRF24L01+ registers and commands
static const uint8_t REG_CONFIG = 0x00;
static const uint8_t REG_EN_AA = 0x01;
static const uint8_t REG_EN_RXADDR = 0x02;
static const uint8_t REG_SETUP_AW = 0x03;
static const uint8_t REG_SETUP_RETR = 0x04;
static const uint8_t REG_RF_CH = 0x05;
static const uint8_t REG_RF_SETUP = 0x06;
static const uint8_t REG_STATUS = 0x07;
static const uint8_t REG_RX_ADDR_P0 = 0x0A;
static const uint8_t REG_TX_ADDR = 0x10;
static const uint8_t REG_RX_PW_P0 = 0x11;
static const uint8_t REG_FIFO_STATUS = 0x17;
static const uint8_t REG_DYNPD = 0x1C;
static const uint8_t REG_FEATURE = 0x1D;
static const uint8_t NRF_R_REGISTER = 0x00;
static const uint8_t NRF_W_REGISTER = 0x20;
static const uint8_t NRF_R_RX_PAYLOAD = 0x61;
static const uint8_t NRF_W_TX_PAYLOAD = 0xA0;
static const uint8_t NRF_FLUSH_TX = 0xE1;
static const uint8_t NRF_FLUSH_RX = 0xE2;
static const uint8_t RF_SETUP_1MBPS_MAX = 0x07;  // 1 Mbit/s, maximum output power

// Colour commands (colour keys, colour cycle) always carry byte 13 = 02 and the lamp only accepts
// them that way. For the other commands the lamp expects either 01 or 02 depending on an internal
// state we could not decode, so those are sent in both variants (the wrong one is ignored).
static bool is_colour_command(uint8_t cmd) {
  return cmd == 0x11 || cmd == 0x15 || cmd == 0x19 || cmd == 0x0A || cmd == 0x02;
}
// White presets of the remote (CC, C/W, WW) arrive with 02 as well but must not change what we learn
static bool is_white_preset(uint8_t cmd) { return cmd == 0x12 || cmd == 0x1A || cmd == 0x03; }

// PL1167 hardware CRC: CRC-16, polynomial 0x1021 reflected (0x8408), init 0, over length + data
static uint16_t crc16_pl1167(const uint8_t *data, size_t len) {
  uint16_t crc = 0;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++)
      crc = (crc & 1) ? (crc >> 1) ^ 0x8408 : crc >> 1;
  }
  return crc;
}

// ---------------------------------------------------------------------------- nRF24 access

uint8_t BrilliantRF::read_reg_(uint8_t reg) {
  this->enable();
  this->transfer_byte(NRF_R_REGISTER | reg);
  uint8_t value = this->transfer_byte(0xFF);
  this->disable();
  return value;
}

void BrilliantRF::write_reg_(uint8_t reg, uint8_t value) {
  this->enable();
  this->transfer_byte(NRF_W_REGISTER | reg);
  this->transfer_byte(value);
  this->disable();
}

void BrilliantRF::write_reg_bytes_(uint8_t reg, const uint8_t *data, size_t len) {
  this->enable();
  this->transfer_byte(NRF_W_REGISTER | reg);
  for (size_t i = 0; i < len; i++)
    this->transfer_byte(data[i]);
  this->disable();
}

void BrilliantRF::command_(uint8_t cmd) {
  this->enable();
  this->transfer_byte(cmd);
  this->disable();
}

bool BrilliantRF::probe_() {
  // Without a module MISO reads 0x00 or 0xFF; a written channel must read back
  this->write_reg_(REG_RF_CH, 0x2A);
  uint8_t a = this->read_reg_(REG_RF_CH);
  this->write_reg_(REG_RF_CH, 0x15);
  uint8_t b = this->read_reg_(REG_RF_CH);
  return a == 0x2A && b == 0x15;
}

void BrilliantRF::configure_rx_() {
  this->ce_pin_->digital_write(false);
  this->write_reg_(REG_CONFIG, 0x03);  // PWR_UP, PRIM_RX, no CRC (PL1167 CRC is checked in software)
  delay(5);
  this->write_reg_(REG_EN_AA, 0x00);
  this->write_reg_(REG_EN_RXADDR, 0x01);
  this->write_reg_(REG_SETUP_AW, 0x01);  // 3-byte address = first three sync word bytes
  this->write_reg_(REG_SETUP_RETR, 0x00);
  this->write_reg_(REG_RF_SETUP, RF_SETUP_1MBPS_MAX);
  this->write_reg_(REG_DYNPD, 0x00);
  this->write_reg_(REG_FEATURE, 0x00);
  this->write_reg_(REG_RX_PW_P0, PAYLOAD);
  uint8_t addr[3] = {this->sync_[2], this->sync_[1], this->sync_[0]};  // register is LSB first
  this->write_reg_bytes_(REG_RX_ADDR_P0, addr, 3);
  this->set_rx_channel_(this->scan_ ? this->scan_first_ : this->channel_);
}

void BrilliantRF::set_rx_channel_(uint8_t channel) {
  this->ce_pin_->digital_write(false);
  this->write_reg_(REG_RF_CH, channel);
  this->command_(NRF_FLUSH_RX);
  this->write_reg_(REG_STATUS, 0x70);
  this->ce_pin_->digital_write(true);
  this->rx_channel_ = channel;
  this->rx_channel_since_ = millis();
}

// ---------------------------------------------------------------------------- setup / loop

void BrilliantRF::add_lamp(const std::vector<uint8_t> &remote_id) {
  Lamp lamp;
  for (int i = 0; i < 5; i++)
    lamp.remote_id[i] = i < (int) remote_id.size() ? remote_id[i] : 0;
  this->lamps_.push_back(lamp);
}

void BrilliantRF::setup() {
  // Counter, remote counter and byte 13 survive reboots, keyed by the remote id
  for (auto &lamp : this->lamps_) {
    uint32_t key = fnv1_hash(format_hex(lamp.remote_id, 5));
    uint8_t stored;
    lamp.counter_pref = global_preferences->make_preference<uint8_t>(key ^ 0x636E7472UL);
    if (lamp.counter_pref.load(&stored))
      lamp.counter = stored;
    lamp.remote_counter_pref = global_preferences->make_preference<uint8_t>(key ^ 0x72656D6FUL);
    lamp.remote_counter = lamp.remote_counter_pref.load(&stored) ? stored : lamp.counter;
    lamp.byte13_pref = global_preferences->make_preference<uint8_t>(key ^ 0x62313300UL);
    if (lamp.byte13_pref.load(&stored))
      lamp.byte13 = stored;
  }
  this->spi_setup();
  this->ce_pin_->setup();
  this->ce_pin_->digital_write(false);
  this->last_probe_ = millis();
  this->found_ = this->probe_();
  if (this->found_) {
    this->configure_rx_();
    this->high_freq_.start();
  } else {
    ESP_LOGW(TAG, "No nRF24 on the SPI bus; retrying every 10 s");
  }
}

void BrilliantRF::loop() {
  uint32_t now = millis();
  if (!this->found_) {
    if (now - this->last_probe_ < 10000)
      return;
    this->last_probe_ = now;
    if (!this->probe_())
      return;
    this->found_ = true;
    ESP_LOGI(TAG, "nRF24 found");
    this->configure_rx_();
    this->high_freq_.start();
  }

  this->receive_();

  if (this->scan_ && now - this->rx_channel_since_ >= this->scan_dwell_ms_) {
    uint8_t next = this->rx_channel_ >= this->scan_last_ ? this->scan_first_ : this->rx_channel_ + 1;
    this->set_rx_channel_(next);
  }

  if (now - this->last_send_ < 150)
    return;
  for (size_t i = 0; i < this->lamps_.size(); i++) {
    this->select_(i);
    Lamp &l = *this->active_;
    bool powered_on = false;
    for (int slot = 0; slot < 2; slot++) {
      Pending &p = l.pending[slot];
      if (!p.open)
        continue;
      p.open = false;
      this->last_send_ = millis();
      if (p.power_on && !l.lamp_on) {
        this->send_selected_(CMD_ON);
        powered_on = true;
      }
      if (slot == 0 && p.command == CMD_OFF) {
        // White off: keep the ring if it should stay on (white at 0 % turns only the white off)
        if (l.rgb_on)
          this->send_selected_(CMD_WHITE, 0, l.white_warm, 100 - l.white_warm, 0);
        else if (l.lamp_on)
          this->send_selected_(CMD_OFF);
        continue;
      }
      if (slot == 1 && p.command == CMD_RGB_OFF) {
        this->send_selected_(CMD_RGB_OFF);
        if (!l.white_on)
          this->send_selected_(CMD_OFF);  // both off: switch the whole lamp off
        continue;
      }
      this->send_selected_(p.command, p.p[0], p.p[1], p.p[2], p.p[3]);
    }
    if (powered_on) {
      // ON always brings the white back and restores the ring: re-apply the wanted state
      if (!l.white_on)
        this->send_selected_(CMD_WHITE, 0, l.white_warm, 100 - l.white_warm, 0);
      if (!l.rgb_on)
        this->send_selected_(CMD_RGB_OFF);
    }
  }
}

// ---------------------------------------------------------------------------- receiving

void BrilliantRF::receive_() {
  for (int round = 0; round < 3; round++) {
    if (this->read_reg_(REG_FIFO_STATUS) & 0x01)  // RX_EMPTY
      return;
    uint8_t payload[PAYLOAD];
    this->enable();
    this->transfer_byte(NRF_R_RX_PAYLOAD);
    this->read_array(payload, PAYLOAD);
    this->disable();
    this->write_reg_(REG_STATUS, 0x40);
    if (this->log_packets_)
      ESP_LOGI(TAG, "Packet ch %u: %s", this->rx_channel_, format_hex(payload, PAYLOAD).c_str());
    this->handle_frame_(payload, this->rx_channel_);
  }
}

void BrilliantRF::handle_frame_(const uint8_t *payload, uint8_t channel) {
  // After the 3-byte address comes the last sync word byte, a 4-bit trailer and then the PL1167
  // frame, each byte sent LSB first
  if (payload[0] != this->sync_[3])
    return;
  uint8_t f[17];
  for (int k = 0; k < 17; k++) {
    uint8_t value = 0;
    for (int j = 0; j < 8; j++) {
      int i = 12 + 8 * k + j;
      if ((payload[i / 8] >> (7 - i % 8)) & 1)
        value |= 1 << j;
    }
    f[k] = value;
  }
  if (f[0] != 0x0E)
    return;
  uint16_t crc = crc16_pl1167(f, 15);
  if ((crc & 0xFF) != f[15] || (crc >> 8) != f[16])
    return;

  size_t nr = 0;
  while (nr < this->lamps_.size() && memcmp(f + 1, this->lamps_[nr].remote_id, 5) != 0)
    nr++;
  if (nr == this->lamps_.size()) {
    // Unknown remote: log once per key press so users can find their remote_id
    static uint8_t last_unknown[6]{};
    if (memcmp(last_unknown, f + 1, 5) != 0 || last_unknown[5] != f[7]) {
      memcpy(last_unknown, f + 1, 5);
      last_unknown[5] = f[7];
      ESP_LOGI(TAG, "Unknown remote, remote_id: [0x%02X, 0x%02X, 0x%02X, 0x%02X, 0x%02X] (channel %u, command %02X)",
               f[1], f[2], f[3], f[4], f[5], channel, f[8]);
    }
    return;
  }

  this->select_(nr);
  Lamp &l = *this->active_;
  if (!is_colour_command(f[8]) && !is_white_preset(f[8]) && f[13] != l.byte13) {
    ESP_LOGD(TAG, "Lamp %u: remote now uses byte 13 = %02X", (unsigned) nr, f[13]);
    l.byte13 = f[13];
    l.byte13_pref.save(&l.byte13);
    global_preferences->sync();
  }
  if (f[7] == l.remote_counter)
    return;  // another copy of a key press we already handled
  ESP_LOGI(TAG, "Lamp %u remote: command %02X values %02X %02X %02X %02X counter %02X (lamp at %02X)", (unsigned) nr,
           f[8], f[9], f[10], f[11], f[12], f[7], l.counter);
  if (f[8] == CMD_ON)
    l.lamp_on = true;
  else if (f[8] == CMD_OFF)
    l.lamp_on = false;
  l.remote_counter = f[7];
  l.remote_counter_pref.save(&l.remote_counter);
  // Remote ahead of us: the lamp accepted it. Remote behind us (we already used its counter
  // values): the lamp drops it, so repeat the same command once with a fresh counter.
  if (f[7] > l.counter) {
    this->store_counter_(f[7]);
    return;
  }
  ESP_LOGI(TAG, "Lamp %u: remote is behind (%02X <= %02X), repeating command %02X", (unsigned) nr, f[7], l.counter,
           f[8]);
  this->byte13_override_ = f[13];
  this->transmit_(this->next_counter_(), f[8], f[9], f[10], f[11], f[12]);
  this->byte13_override_ = -1;
}

// ---------------------------------------------------------------------------- counter

bool BrilliantRF::select_(uint8_t lamp) {
  if (lamp >= this->lamps_.size())
    return false;
  this->active_ = &this->lamps_[lamp];
  return true;
}

uint8_t BrilliantRF::next_counter_() {
  // Like the remote: every counter value once, always +1. Before the lamp's missing wrap-around
  // becomes a problem, step the lamp down.
  if (this->active_->counter >= 0xF0)
    this->lower_counter_();
  return this->active_->counter + 1;
}

void BrilliantRF::lower_counter_() {
  // Go back in steps of 32 (the lamp accepts ~40 back), re-sending the current state so nothing
  // visibly changes. Each step uses both byte 13 variants (z-1, then z), so it ends on z and the
  // next packet (z + 1) is guaranteed fresh.
  Lamp &l = *this->active_;
  ESP_LOGI(TAG, "Counter at %02X: stepping the lamp down before the wrap-around", l.counter);
  int saved_override = this->byte13_override_;
  uint8_t first = l.byte13, second = first == 0x01 ? 0x02 : 0x01;
  while (l.counter > 0x30) {
    uint8_t z = l.counter - 33;
    for (uint8_t b13 : {first, second}) {
      this->byte13_override_ = b13;
      if (l.lamp_on)
        this->transmit_(z, CMD_WHITE, l.white_on ? l.white_level : 0, l.white_warm, 100 - l.white_warm, 0);
      else
        this->transmit_(z, CMD_OFF, 0, 0, 0, 0);
      z++;
    }
  }
  this->byte13_override_ = saved_override;
}

void BrilliantRF::store_counter_(uint8_t value) {
  this->active_->counter = value;
  this->active_->counter_pref.save(&this->active_->counter);
  global_preferences->sync();
}

// ---------------------------------------------------------------------------- sending

void BrilliantRF::queue(uint8_t lamp, uint8_t slot, uint8_t command, uint8_t p1, uint8_t p2, uint8_t p3, uint8_t p4,
                        bool power_on) {
  if (lamp >= this->lamps_.size() || slot > 1)
    return;
  Lamp &l = this->lamps_[lamp];
  if (slot == 0) {
    l.white_on = command != CMD_OFF;
    if (command == CMD_WHITE) {
      l.white_level = p1;
      l.white_warm = p2;
    }
  } else {
    l.rgb_on = command != CMD_RGB_OFF;
  }
  l.pending[slot] = {true, command, {p1, p2, p3, p4}, power_on};
}

void BrilliantRF::send(uint8_t lamp, uint8_t command, uint8_t p1, uint8_t p2, uint8_t p3, uint8_t p4) {
  if (this->select_(lamp))
    this->send_selected_(command, p1, p2, p3, p4);
}

void BrilliantRF::send_raw(uint8_t lamp, uint8_t command, uint8_t p1, uint8_t p2, uint8_t p3, uint8_t p4,
                           uint8_t counter, uint8_t byte13) {
  if (!this->select_(lamp))
    return;
  this->byte13_override_ = byte13;
  this->transmit_(counter, command, p1, p2, p3, p4);
  this->byte13_override_ = -1;
}

void BrilliantRF::set_counter(uint8_t lamp, uint8_t value) {
  if (this->select_(lamp))
    this->store_counter_(value);
}

void BrilliantRF::send_selected_(uint8_t command, uint8_t p1, uint8_t p2, uint8_t p3, uint8_t p4) {
  if (this->byte13_override_ >= 0 || is_colour_command(command)) {
    this->transmit_(this->next_counter_(), command, p1, p2, p3, p4);
    return;
  }
  // Byte 13 for non-colour commands flips between 01 and 02 inside the lamp: send both. These
  // commands set absolute values, so applying one twice does no harm.
  uint8_t first = this->active_->byte13, second = first == 0x01 ? 0x02 : 0x01;
  for (uint8_t b13 : {first, second}) {
    this->byte13_override_ = b13;
    this->transmit_(this->next_counter_(), command, p1, p2, p3, p4);
  }
  this->byte13_override_ = -1;
}

void BrilliantRF::transmit_(uint8_t counter, uint8_t command, uint8_t p1, uint8_t p2, uint8_t p3, uint8_t p4) {
  if (!this->found_ || this->active_ == nullptr) {
    ESP_LOGW(TAG, "Cannot send: no nRF24 or no lamp");
    return;
  }
  Lamp &l = *this->active_;
  uint8_t frame[17];
  frame[0] = 0x0E;  // length
  memcpy(frame + 1, l.remote_id, 5);
  frame[6] = 0x00;
  frame[7] = counter;
  frame[8] = command;
  frame[9] = p1;
  frame[10] = p2;
  frame[11] = p3;
  frame[12] = p4;
  if (this->byte13_override_ >= 0)
    frame[13] = this->byte13_override_;
  else
    frame[13] = is_colour_command(command) ? 0x02 : l.byte13;
  uint8_t sum = 0;
  for (int i = 1; i <= 12; i++)
    sum += frame[i];
  frame[14] = sum;
  uint16_t crc = crc16_pl1167(frame, 15);
  frame[15] = crc & 0xFF;
  frame[16] = crc >> 8;
  this->store_counter_(counter);
  if (command == CMD_ON)
    l.lamp_on = true;
  else if (command == CMD_OFF)
    l.lamp_on = false;

  // Bit stream after the address: last sync word byte (MSB first), trailer 0101, frame LSB first
  uint8_t payload[PAYLOAD] = {0};
  size_t bit = 0;
  auto put = [&](bool value) {
    if (value)
      payload[bit / 8] |= 0x80 >> (bit % 8);
    bit++;
  };
  for (int i = 7; i >= 0; i--)
    put((this->sync_[3] >> i) & 1);
  for (bool b : {false, true, false, true})
    put(b);
  for (uint8_t byte : frame)
    for (int i = 0; i < 8; i++)
      put((byte >> i) & 1);
  size_t len = (bit + 7) / 8 + 1;

  // TX: 5-byte address = two preamble bytes + first three sync word bytes, no CRC, no ack
  this->ce_pin_->digital_write(false);
  this->write_reg_(REG_CONFIG, 0x02);  // PWR_UP, PRIM_TX, no CRC
  this->write_reg_(REG_SETUP_AW, 0x03);
  uint8_t addr[5] = {this->sync_[2], this->sync_[1], this->sync_[0], 0xAA, 0xAA};  // LSB first
  this->write_reg_bytes_(REG_TX_ADDR, addr, 5);
  this->write_reg_(REG_RF_CH, this->channel_);
  this->write_reg_(REG_RF_SETUP, RF_SETUP_1MBPS_MAX);
  delayMicroseconds(150);
  uint8_t sent = 0;
  for (uint8_t n = 0; n < this->repeats_; n++) {
    this->command_(NRF_FLUSH_TX);
    this->write_reg_(REG_STATUS, 0x70);
    this->enable();
    this->transfer_byte(NRF_W_TX_PAYLOAD);
    for (size_t i = 0; i < len; i++)
      this->transfer_byte(payload[i]);
    this->disable();
    this->ce_pin_->digital_write(true);
    delayMicroseconds(15);
    this->ce_pin_->digital_write(false);
    delayMicroseconds(1200);  // ~130 us start-up + ~200 us on air; the remote sends about every 1 ms
    if (this->read_reg_(REG_STATUS) & 0x20)
      sent++;
  }
  ESP_LOGD(TAG, "Sent to %s: command %02X values %02X %02X %02X %02X counter %02X byte13 %02X (%u/%u)",
           format_hex_pretty(l.remote_id, 5).c_str(), command, p1, p2, p3, p4, counter, frame[13], sent,
           this->repeats_);
  this->configure_rx_();
}

void BrilliantRF::dump_config() {
  ESP_LOGCONFIG(TAG, "Brilliant RF:");
  LOG_PIN("  CE: ", this->ce_pin_);
  ESP_LOGCONFIG(TAG, "  nRF24 found: %s", YESNO(this->found_));
  ESP_LOGCONFIG(TAG, "  Channel: %u, sync word: %s, repeats: %u", this->channel_,
                format_hex_pretty(this->sync_, 4).c_str(), this->repeats_);
  if (this->scan_)
    ESP_LOGCONFIG(TAG, "  Scanning channels %u..%u", this->scan_first_, this->scan_last_);
  for (size_t i = 0; i < this->lamps_.size(); i++)
    ESP_LOGCONFIG(TAG, "  Lamp %u: remote_id %s, counter %02X", (unsigned) i,
                  format_hex_pretty(this->lamps_[i].remote_id, 5).c_str(), this->lamps_[i].counter);
}

}  // namespace brilliant_rf
}  // namespace esphome
