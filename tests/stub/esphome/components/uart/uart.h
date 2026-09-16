#pragma once
#include <cstdint>
#include <cstddef>
namespace esphome {
namespace uart {
class UARTDevice {
 public:
  int available();
  uint8_t read();
  void write_array(const uint8_t *data, size_t len);
  bool check_uart_settings(uint32_t baud_rate);
};
}  // namespace uart
}  // namespace esphome
