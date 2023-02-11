#include "hink_epaper.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"
#include "esphome/core/helpers.h"

namespace esphome {
namespace hink_epaper {

static const char *const TAG = "hink_epaper";

static const uint8_t LUT_SIZE_HINK = 30;

static const uint8_t FULL_UPDATE_LUT[LUT_SIZE_HINK] = {0x02, 0x02, 0x01, 0x11, 0x12, 0x12, 0x22, 0x22, 0x66, 0x69,
                                                       0x69, 0x59, 0x58, 0x99, 0x99, 0x88, 0x00, 0x00, 0x00, 0x00,
                                                       0xF8, 0xB4, 0x13, 0x51, 0x35, 0x51, 0x51, 0x19, 0x01, 0x00};

static const uint8_t PARTIAL_UPDATE_LUT[LUT_SIZE_HINK] = {
    0x10, 0x18, 0x18, 0x08, 0x18, 0x18, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x13, 0x14, 0x44, 0x12, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

void HinkEPaper::setup_pins_() {
  this->init_internal_(this->get_buffer_length_());
  this->dc_pin_->setup();  // OUTPUT
  this->dc_pin_->digital_write(false);
  if (this->reset_pin_ != nullptr) {
    this->reset_pin_->setup();  // OUTPUT
    this->reset_pin_->digital_write(true);
  }
  if (this->busy_pin_ != nullptr) {
    this->busy_pin_->setup();  // INPUT
  }
  this->spi_setup();

  this->reset_();
}
float HinkEPaper::get_setup_priority() const { return setup_priority::PROCESSOR; }
void HinkEPaper::command(uint8_t value) {
  this->start_command_();
  this->write_byte(value);
  this->end_command_();
}
void HinkEPaper::data(uint8_t value) {
  this->start_data_();
  this->write_byte(value);
  this->end_data_();
}
bool HinkEPaper::wait_until_idle_() {
  if (this->busy_pin_ == nullptr) {
    return true;
  }

  const uint32_t start = millis();
  while (this->busy_pin_->digital_read()) {
    if (millis() - start > this->idle_timeout_()) {
      ESP_LOGE(TAG, "Timeout while displaying image!");
      return false;
    }
    delay(10);
  }
  return true;
}
void HinkEPaper::update() {
  this->do_update_();
  this->display();
}
void HinkEPaper::fill(Color color) {
  // flip logic
  const uint8_t fill = color.is_on() ? 0x00 : 0xFF;
  for (uint32_t i = 0; i < this->get_buffer_length_(); i++)
    this->buffer_[i] = fill;
}
void HOT HinkEPaper::draw_absolute_pixel_internal(int x, int y, Color color) {
  if (x >= this->get_width_internal() || y >= this->get_height_internal() || x < 0 || y < 0)
    return;

  const uint32_t pos = (x + y * this->get_width_internal()) / 8u;
  const uint8_t subpos = x & 0x07;
  // flip logic
  if (!color.is_on()) {
    this->buffer_[pos] |= 0x80 >> subpos;
  } else {
    this->buffer_[pos] &= ~(0x80 >> subpos);
  }
}
uint32_t HinkEPaper::get_buffer_length_() { return this->get_width_internal() * this->get_height_internal() / 8u; }
void HinkEPaper::start_command_() {
  this->dc_pin_->digital_write(false);
  this->enable();
}
void HinkEPaper::end_command_() { this->disable(); }
void HinkEPaper::start_data_() {
  this->dc_pin_->digital_write(true);
  this->enable();
}
void HinkEPaper::end_data_() { this->disable(); }
void HinkEPaper::on_safe_shutdown() { this->deep_sleep(); }

// ========================================================
//                          Model 1.54
// ========================================================

void HinkEPaper1P54IN::initialize() {
  // COMMAND DRIVER OUTPUT CONTROL
  this->command(0x01);
  this->data(this->get_height_internal() - 1);
  this->data((this->get_height_internal() - 1) >> 8);
  this->data(0x00);  // ? GD = 0, SM = 0, TB = 0

  // COMMAND BOOSTER SOFT START CONTROL
  this->command(0x0C);
  this->data(0xD7);
  this->data(0xD6);
  this->data(0x9D);

  // COMMAND WRITE VCOM REGISTER
  this->command(0x2C);
  this->data(0xA8);

  // COMMAND SET DUMMY LINE PERIOD
  this->command(0x3A);
  this->data(0x1A);

  // COMMAND SET GATE TIME
  this->command(0x3B);
  this->data(0x08);  // 2µs per row

  // COMMAND DATA ENTRY MODE SETTING
  this->command(0x11);
  this->data(0x03);  // from top left to bottom right
}
void HinkEPaper1P54IN::dump_config() {
  LOG_DISPLAY("", "Hink E-Paper", this);
  ESP_LOGCONFIG(TAG, "  Model: 1.54in");
  ESP_LOGCONFIG(TAG, "  Full Update Every: %u", this->full_update_every_);
  LOG_PIN("  Reset Pin: ", this->reset_pin_);
  LOG_PIN("  DC Pin: ", this->dc_pin_);
  LOG_PIN("  Busy Pin: ", this->busy_pin_);
  LOG_UPDATE_INTERVAL(this);
}
void HOT HinkEPaper1P54IN::display() {
  bool full_update = this->at_update_ == 0;
  bool prev_full_update = this->at_update_ == 1;

  if (!this->wait_until_idle_()) {
    this->status_set_warning();
    return;
  }

  if (this->full_update_every_ >= 1) {
    if (full_update != prev_full_update) {
      this->write_lut_(full_update ? FULL_UPDATE_LUT : PARTIAL_UPDATE_LUT, LUT_SIZE_HINK);
    }
    this->at_update_ = (this->at_update_ + 1) % this->full_update_every_;
  }

  // Set x & y regions we want to write to (full)
  // COMMAND SET RAM X ADDRESS START END POSITION
  this->command(0x44);
  this->data(0x00);
  this->data((this->get_width_internal() - 1) >> 3);
  // COMMAND SET RAM Y ADDRESS START END POSITION
  this->command(0x45);
  this->data(0x00);
  this->data(0x00);
  this->data(this->get_height_internal() - 1);
  this->data((this->get_height_internal() - 1) >> 8);

  // COMMAND SET RAM X ADDRESS COUNTER
  this->command(0x4E);
  this->data(0x00);
  // COMMAND SET RAM Y ADDRESS COUNTER
  this->command(0x4F);
  this->data(0x00);
  this->data(0x00);
  
  if (!this->wait_until_idle_()) {
    this->status_set_warning();
    return;
  }

  // COMMAND WRITE RAM
  this->command(0x24);
  this->start_data_();
  this->write_array(this->buffer_, this->get_buffer_length_());
  this->end_data_();

  // COMMAND DISPLAY UPDATE CONTROL 2
  this->command(0x22);
  this->data(0xC4);

  // COMMAND MASTER ACTIVATION
  this->command(0x20);
  // COMMAND TERMINATE FRAME READ WRITE
  this->command(0xFF);

  this->status_clear_warning();
}
int HinkEPaper1P54IN::get_width_internal() {
  return 200;
}
int HinkEPaper1P54IN::get_height_internal() {
  return 200;
}
void HinkEPaper1P54IN::write_lut_(const uint8_t *lut, const uint8_t size) {
  // COMMAND WRITE LUT REGISTER
  this->command(0x32);
  for (uint8_t i = 0; i < size; i++)
    this->data(lut[i]);
}
HinkEPaper1P54IN::HinkEPaper(HinkEPaperModel model) : model_(model) {}
void HinkEPaper::set_full_update_every(uint32_t full_update_every) {
  this->full_update_every_ = full_update_every;
}

uint32_t HinkEPaper1P54IN::idle_timeout_() {
  return HinkEPaper::idle_timeout_();
}

// ========================================================
//                          Model 2.9
// ========================================================

void HinkEPaper2P9IN::initialize() {
  // COMMAND DRIVER OUTPUT CONTROL
  this->command(0x01);
  this->data(this->get_height_internal() - 1);
  this->data((this->get_height_internal() - 1) >> 8);
  this->data(0x00);  // ? GD = 0, SM = 0, TB = 0

  // COMMAND BOOSTER SOFT START CONTROL
  this->command(0x0C);
  this->data(0xD7);
  this->data(0xD6);
  this->data(0x9D);

  // COMMAND WRITE VCOM REGISTER
  this->command(0x2C);
  this->data(0xA8);

  // COMMAND SET DUMMY LINE PERIOD
  this->command(0x3A);
  this->data(0x1A);

  // COMMAND SET GATE TIME
  this->command(0x3B);
  this->data(0x08);  // 2µs per row

  // COMMAND DATA ENTRY MODE SETTING
  this->command(0x11);
  this->data(0x03);  // from top left to bottom right
}
void HinkEPaper2P9IN::dump_config() {
  LOG_DISPLAY("", "Hink E-Paper", this);
  ESP_LOGCONFIG(TAG, "  Model: 2.9in");
  ESP_LOGCONFIG(TAG, "  Full Update Every: %u", this->full_update_every_);
  LOG_PIN("  Reset Pin: ", this->reset_pin_);
  LOG_PIN("  DC Pin: ", this->dc_pin_);
  LOG_PIN("  Busy Pin: ", this->busy_pin_);
  LOG_UPDATE_INTERVAL(this);
}
void HOT HinkEPaper2P9IN::display() {
  bool full_update = this->at_update_ == 0;
  bool prev_full_update = this->at_update_ == 1;

  if (!this->wait_until_idle_()) {
    this->status_set_warning();
    return;
  }

  if (this->full_update_every_ >= 1) {
    if (full_update != prev_full_update) {
      this->write_lut_(full_update ? FULL_UPDATE_LUT : PARTIAL_UPDATE_LUT, LUT_SIZE_HINK);
    }
    this->at_update_ = (this->at_update_ + 1) % this->full_update_every_;
  }

  // Set x & y regions we want to write to (full)
  // COMMAND SET RAM X ADDRESS START END POSITION
  this->command(0x44);
  this->data(0x00);
  this->data((this->get_width_internal() - 1) >> 3);
  // COMMAND SET RAM Y ADDRESS START END POSITION
  this->command(0x45);
  this->data(0x00);
  this->data(0x00);
  this->data(this->get_height_internal() - 1);
  this->data((this->get_height_internal() - 1) >> 8);

  // COMMAND SET RAM X ADDRESS COUNTER
  this->command(0x4E);
  this->data(0x00);
  // COMMAND SET RAM Y ADDRESS COUNTER
  this->command(0x4F);
  this->data(0x00);
  this->data(0x00);
  
  if (!this->wait_until_idle_()) {
    this->status_set_warning();
    return;
  }

  // COMMAND WRITE RAM
  this->command(0x24);
  this->start_data_();
  this->write_array(this->buffer_, this->get_buffer_length_());
  this->end_data_();

  // COMMAND DISPLAY UPDATE CONTROL 2
  this->command(0x22);
  this->data(0xC4);

  // COMMAND MASTER ACTIVATION
  this->command(0x20);
  // COMMAND TERMINATE FRAME READ WRITE
  this->command(0xFF);

  this->status_clear_warning();
}
int HinkEPaper2P9IN::get_width_internal() {
  return 128;
}
int HinkEPaper2P9IN::get_height_internal() {
  return 296;
}
void HinkEPaper2P9IN::write_lut_(const uint8_t *lut, const uint8_t size) {
  // COMMAND WRITE LUT REGISTER
  this->command(0x32);
  for (uint8_t i = 0; i < size; i++)
    this->data(lut[i]);
}
HinkEPaper2P9IN::HinkEPaper2P9IN(HinkEPaperModel model) : model_(model) {}
void HinkEPaper2P9IN::set_full_update_every(uint32_t full_update_every) {
  this->full_update_every_ = full_update_every;
}

uint32_t HinkEPaper2P9IN::idle_timeout_() {
  return HinkEPaper::idle_timeout_();
}

// ========================================================
//                          Model 4.2
// ========================================================

static const uint8_t LUT_VCOM_DC_4_2[] = {
    0x00, 0x17, 0x00, 0x00, 0x00, 0x02, 0x00, 0x17, 0x17, 0x00, 0x00, 0x02, 0x00, 0x0A, 0x01,
    0x00, 0x00, 0x01, 0x00, 0x0E, 0x0E, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
static const uint8_t LUT_WHITE_TO_WHITE_4_2[] = {
    0x40, 0x17, 0x00, 0x00, 0x00, 0x02, 0x90, 0x17, 0x17, 0x00, 0x00, 0x02, 0x40, 0x0A,
    0x01, 0x00, 0x00, 0x01, 0xA0, 0x0E, 0x0E, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
static const uint8_t LUT_BLACK_TO_WHITE_4_2[] = {
    0x40, 0x17, 0x00, 0x00, 0x00, 0x02, 0x90, 0x17, 0x17, 0x00, 0x00, 0x02, 0x40, 0x0A,
    0x01, 0x00, 0x00, 0x01, 0xA0, 0x0E, 0x0E, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static const uint8_t LUT_BLACK_TO_BLACK_4_2[] = {
    0x80, 0x17, 0x00, 0x00, 0x00, 0x02, 0x90, 0x17, 0x17, 0x00, 0x00, 0x02, 0x80, 0x0A,
    0x01, 0x00, 0x00, 0x01, 0x50, 0x0E, 0x0E, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static const uint8_t LUT_WHITE_TO_BLACK_4_2[] = {
    0x80, 0x17, 0x00, 0x00, 0x00, 0x02, 0x90, 0x17, 0x17, 0x00, 0x00, 0x02, 0x80, 0x0A,
    0x01, 0x00, 0x00, 0x01, 0x50, 0x0E, 0x0E, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

void HinkEPaper4P2In::initialize() {

  // COMMAND POWER SETTING
  this->command(0x01);
  this->data(0x03);  // VDS_EN, VDG_EN
  this->data(0x00);  // VCOM_HV, VGHL_LV[1], VGHL_LV[0]
  this->data(0x2B);  // VDH
  this->data(0x2B);  // VDL
  this->data(0xFF);  // VDHR

  // COMMAND BOOSTER SOFT START
  this->command(0x06);
  this->data(0x17);  // PHA
  this->data(0x17);  // PHB
  this->data(0x17);  // PHC

  // COMMAND POWER ON
  this->command(0x04);
  this->wait_until_idle_();
  delay(10);
  // COMMAND PANEL SETTING
  this->command(0x00);
  this->data(0xBF);  // KW-BF   KWR-AF  BWROTP 0f
  this->data(0x0B);
  // COMMAND PLL CONTROL
  this->command(0x30);
  this->data(0x3C);  // 3A 100HZ   29 150Hz 39 200HZ  31 171HZ

  delay(2);
  // COMMAND LUT FOR VCOM
  this->command(0x20);
  for (uint8_t i : LUT_VCOM_DC_4_2)
    this->data(i);
  // COMMAND LUT WHITE TO WHITE
  this->command(0x21);
  for (uint8_t i : LUT_WHITE_TO_WHITE_4_2)
    this->data(i);
  // COMMAND LUT BLACK TO WHITE
  this->command(0x22);
  for (uint8_t i : LUT_BLACK_TO_WHITE_4_2)
    this->data(i);
  // COMMAND LUT WHITE TO BLACK
  this->command(0x23);
  for (uint8_t i : LUT_WHITE_TO_BLACK_4_2)
    this->data(i);
  // COMMAND LUT BLACK TO BLACK
  this->command(0x24);
  for (uint8_t i : LUT_BLACK_TO_BLACK_4_2)
    this->data(i);
}
void HOT HinkEPaper4P2In::display() {
  // COMMAND RESOLUTION SETTING
  this->command(0x61);
  this->data(0x01);
  this->data(0x90);
  this->data(0x01);
  this->data(0x2C);

  // COMMAND VCM DC SETTING REGISTER
  this->command(0x82);
  this->data(0x12);

  // COMMAND VCOM AND DATA INTERVAL SETTING
  this->command(0x50);
  this->data(0x97);

  // COMMAND DATA START TRANSMISSION 1
  this->command(0x10);
  delay(2);
  this->start_data_();
  this->write_array(this->buffer_, this->get_buffer_length_());
  this->end_data_();
  delay(2);
  // COMMAND DATA START TRANSMISSION 2
  this->command(0x13);
  delay(2);
  this->start_data_();
  this->write_array(this->buffer_, this->get_buffer_length_());
  this->end_data_();
  // COMMAND DISPLAY REFRESH
  this->command(0x12);
}
int HinkEPaper4P2In::get_width_internal() { return 400; }
int HinkEPaper4P2In::get_height_internal() { return 300; }
void HinkEPaper4P2In::dump_config() {
  LOG_DISPLAY("", "Hink E-Paper", this);
  ESP_LOGCONFIG(TAG, "  Model: 4.2in");
  LOG_PIN("  Reset Pin: ", this->reset_pin_);
  LOG_PIN("  DC Pin: ", this->dc_pin_);
  LOG_PIN("  Busy Pin: ", this->busy_pin_);
  LOG_UPDATE_INTERVAL(this);
}

}  // namespace hink_epaper
}  // namespace esphome