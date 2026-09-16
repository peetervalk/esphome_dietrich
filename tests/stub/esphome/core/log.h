#pragma once
namespace esphome {
void esph_log(const char *tag, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
}
#define ESP_LOGD(tag, ...) ::esphome::esph_log(tag, __VA_ARGS__)
#define ESP_LOGI(tag, ...) ::esphome::esph_log(tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) ::esphome::esph_log(tag, __VA_ARGS__)
#define ESP_LOGE(tag, ...) ::esphome::esph_log(tag, __VA_ARGS__)
#define ESP_LOGCONFIG(tag, ...) ::esphome::esph_log(tag, __VA_ARGS__)
#define LOG_UPDATE_INTERVAL(obj) ((void) (obj))
