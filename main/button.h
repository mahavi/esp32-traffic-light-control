#ifndef BUTTON_H_
#define BUTTON_H_

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_timer.h"

typedef struct
{
  gpio_num_t gpio;
  uint64_t debounce_us;
  esp_timer_handle_t timer;
  volatile bool pressed;
} button_t;

/* Call once before initializing any buttons. */
void button_service_init(void);

/* Initializes an active-low button with the internal pull-up enabled. */
void button_init(
    button_t *button,
    gpio_num_t gpio,
    uint64_t debounce_us,
    const char *name);

/* Returns true once for each debounced press accepted by the module. */
bool button_take_press(button_t *button);

#endif // BUTTON_H_
