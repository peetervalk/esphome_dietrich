#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
namespace esphome {
namespace setup_priority { const float DATA = 600.0f; }
class Component {
 public:
  virtual ~Component() = default;
  virtual void setup() {}
  virtual void loop() {}
  virtual void dump_config() {}
  virtual float get_setup_priority() const { return 0.0f; }
};
class PollingComponent : public Component {
 public:
  virtual void update() = 0;
  uint32_t get_update_interval() const { return update_interval_; }
  void set_update_interval(uint32_t i) { update_interval_ = i; }
 protected:
  uint32_t update_interval_{0};
};
}  // namespace esphome

#define SUB_SENSOR(name) \
 protected: \
  sensor::Sensor *name##_sensor_{nullptr}; \
 public: \
  void set_##name##_sensor(sensor::Sensor *s) { this->name##_sensor_ = s; }

#define SUB_BINARY_SENSOR(name) \
 protected: \
  binary_sensor::BinarySensor *name##_binary_sensor_{nullptr}; \
 public: \
  void set_##name##_binary_sensor(binary_sensor::BinarySensor *s) { this->name##_binary_sensor_ = s; }

#define SUB_TEXT_SENSOR(name) \
 protected: \
  text_sensor::TextSensor *name##_text_sensor_{nullptr}; \
 public: \
  void set_##name##_text_sensor(text_sensor::TextSensor *s) { this->name##_text_sensor_ = s; }
