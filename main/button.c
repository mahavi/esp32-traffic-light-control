#include "button.h"

#include "esp_attr.h"
#include "esp_err.h"

void button_service_init(void)
{
  ESP_ERROR_CHECK(gpio_install_isr_service(ESP_INTR_FLAG_IRAM));
}

static void IRAM_ATTR button_gpio_isr_handler(void *arg)
{
  button_t *button = arg;

  esp_timer_stop(button->timer);
  esp_timer_start_once(button->timer, button->debounce_us);
}

static void button_debounce_timer_cb(void *arg)
{
  button_t *button = arg;

  if (gpio_get_level(button->gpio) != 0)
  {
    return;
  }

  button->pressed = true;
}

void button_init(
    button_t *button,
    gpio_num_t gpio,
    uint64_t debounce_us,
    const char *name)
{
  button->gpio = gpio;
  button->debounce_us = debounce_us;
  button->pressed = false;

  gpio_config_t io = {
      .pin_bit_mask = 1ULL << button->gpio,
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_NEGEDGE,
  };
  ESP_ERROR_CHECK(gpio_config(&io));

  const esp_timer_create_args_t timer_args = {
      .callback = button_debounce_timer_cb,
      .arg = button,
      .dispatch_method = ESP_TIMER_TASK,
      .name = name,
  };
  ESP_ERROR_CHECK(esp_timer_create(&timer_args, &button->timer));

  ESP_ERROR_CHECK(gpio_isr_handler_add(
      button->gpio, button_gpio_isr_handler, button));
}

bool button_take_press(button_t *button)
{
  bool pressed = button->pressed;
  button->pressed = false;
  return pressed;
}
