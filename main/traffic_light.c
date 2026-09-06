#include "traffic_light.h"

#include <stdbool.h>
#include <stdint.h>

#include "configuration.h"
#include "driver/gpio.h"
#include "driver/gptimer.h"
#include "esp_attr.h"
#include "esp_err.h"

#define MILLISECONDS_TO_TICKS(milliseconds) \
  ((uint64_t)(milliseconds) * TRAFFIC_LIGHT_TIMER_RESOLUTION_HZ / 1000U)

typedef enum
{
  TRAFFIC_LIGHT_STATE_GREEN,
  TRAFFIC_LIGHT_STATE_GREEN_BLINKING,
  TRAFFIC_LIGHT_STATE_YELLOW,
  TRAFFIC_LIGHT_STATE_RED,
  TRAFFIC_LIGHT_STATE_RED_YELLOW
} traffic_light_state_t;

static gptimer_handle_t s_state_timer;
static gptimer_handle_t s_blink_timer;

static volatile bool s_state_timer_alarm_pending;
static volatile bool s_blink_timer_alarm_pending;
static traffic_light_state_t s_current_state = TRAFFIC_LIGHT_STATE_GREEN;
static traffic_light_mode_t s_current_mode = TRAFFIC_LIGHT_MODE_OFF;

static bool IRAM_ATTR on_state_timer_alarm(
    gptimer_handle_t timer,
    const gptimer_alarm_event_data_t *event_data,
    void *user_context)
{
  (void)event_data;
  (void)user_context;

  s_state_timer_alarm_pending = true;
  gptimer_stop(timer);

  return false;
}

static bool IRAM_ATTR on_blink_timer_alarm(
    gptimer_handle_t timer,
    const gptimer_alarm_event_data_t *event_data,
    void *user_context)
{
  (void)timer;
  (void)event_data;
  (void)user_context;

  s_blink_timer_alarm_pending = true;

  return false;
}

static void start_state_timer(uint64_t duration_ticks)
{
  ESP_ERROR_CHECK(gptimer_stop(s_state_timer));
  ESP_ERROR_CHECK(gptimer_set_raw_count(s_state_timer, 0));

  gptimer_alarm_config_t alarm = {
      .alarm_count = duration_ticks,
      .flags.auto_reload_on_alarm = false,
  };

  ESP_ERROR_CHECK(gptimer_set_alarm_action(s_state_timer, &alarm));
  ESP_ERROR_CHECK(gptimer_start(s_state_timer));
}

static void stop_state_timer(void)
{
  ESP_ERROR_CHECK(gptimer_stop(s_state_timer));
  ESP_ERROR_CHECK(gptimer_set_raw_count(s_state_timer, 0));

  s_state_timer_alarm_pending = false;
}

static void start_blink_timer(uint64_t duration_ticks)
{
  ESP_ERROR_CHECK(gptimer_stop(s_blink_timer));
  ESP_ERROR_CHECK(gptimer_set_raw_count(s_blink_timer, 0));

  gptimer_alarm_config_t alarm = {
      .alarm_count = duration_ticks,
      .flags.auto_reload_on_alarm = true,
  };

  ESP_ERROR_CHECK(gptimer_set_alarm_action(s_blink_timer, &alarm));
  ESP_ERROR_CHECK(gptimer_start(s_blink_timer));
}

static void stop_blink_timer(void)
{
  ESP_ERROR_CHECK(gptimer_stop(s_blink_timer));
  ESP_ERROR_CHECK(gptimer_set_raw_count(s_blink_timer, 0));

  s_blink_timer_alarm_pending = false;
}

static void set_lights(bool red_on, bool yellow_on, bool green_on)
{
  gpio_set_level(TRAFFIC_LIGHT_RED_LED_GPIO, red_on);
  gpio_set_level(TRAFFIC_LIGHT_YELLOW_LED_GPIO, yellow_on);
  gpio_set_level(TRAFFIC_LIGHT_GREEN_LED_GPIO, green_on);
}

static void enter_state(traffic_light_state_t new_state)
{
  s_current_state = new_state;

  switch (s_current_state)
  {
  case TRAFFIC_LIGHT_STATE_GREEN:
    set_lights(false, false, true);
    start_state_timer(
        MILLISECONDS_TO_TICKS(TRAFFIC_LIGHT_GREEN_DURATION_MS));
    break;

  case TRAFFIC_LIGHT_STATE_GREEN_BLINKING:
    set_lights(false, false, true);
    start_state_timer(
        MILLISECONDS_TO_TICKS(TRAFFIC_LIGHT_GREEN_BLINK_DURATION_MS));
    start_blink_timer(
        MILLISECONDS_TO_TICKS(TRAFFIC_LIGHT_BLINK_INTERVAL_MS));
    break;

  case TRAFFIC_LIGHT_STATE_YELLOW:
    set_lights(false, true, false);
    start_state_timer(
        MILLISECONDS_TO_TICKS(TRAFFIC_LIGHT_YELLOW_DURATION_MS));
    break;

  case TRAFFIC_LIGHT_STATE_RED:
    set_lights(true, false, false);
    start_state_timer(
        MILLISECONDS_TO_TICKS(TRAFFIC_LIGHT_RED_DURATION_MS));
    break;

  case TRAFFIC_LIGHT_STATE_RED_YELLOW:
    set_lights(true, true, false);
    start_state_timer(
        MILLISECONDS_TO_TICKS(TRAFFIC_LIGHT_RED_YELLOW_DURATION_MS));
    break;
  }
}

static void handle_state_timer_alarm(void)
{
  switch (s_current_state)
  {
  case TRAFFIC_LIGHT_STATE_GREEN:
    enter_state(TRAFFIC_LIGHT_STATE_GREEN_BLINKING);
    break;

  case TRAFFIC_LIGHT_STATE_GREEN_BLINKING:
    stop_blink_timer();
    enter_state(TRAFFIC_LIGHT_STATE_YELLOW);
    break;

  case TRAFFIC_LIGHT_STATE_YELLOW:
    enter_state(TRAFFIC_LIGHT_STATE_RED);
    break;

  case TRAFFIC_LIGHT_STATE_RED:
    enter_state(TRAFFIC_LIGHT_STATE_RED_YELLOW);
    break;

  case TRAFFIC_LIGHT_STATE_RED_YELLOW:
    enter_state(TRAFFIC_LIGHT_STATE_GREEN);
    break;
  }
}

static void handle_blink_timer_alarm(void)
{
  if (s_current_mode == TRAFFIC_LIGHT_MODE_YELLOW_BLINKING)
  {
    set_lights(
        false,
        !gpio_get_level(TRAFFIC_LIGHT_YELLOW_LED_GPIO),
        false);
  }
  else if (s_current_mode == TRAFFIC_LIGHT_MODE_NORMAL &&
           s_current_state == TRAFFIC_LIGHT_STATE_GREEN_BLINKING)
  {
    set_lights(
        false,
        false,
        !gpio_get_level(TRAFFIC_LIGHT_GREEN_LED_GPIO));
  }
}

static void setup_state_timer(void)
{
  gptimer_config_t config = {
      .clk_src = GPTIMER_CLK_SRC_DEFAULT,
      .direction = GPTIMER_COUNT_UP,
      .resolution_hz = TRAFFIC_LIGHT_TIMER_RESOLUTION_HZ,
  };
  ESP_ERROR_CHECK(gptimer_new_timer(&config, &s_state_timer));

  gptimer_event_callbacks_t callbacks = {
      .on_alarm = on_state_timer_alarm,
  };
  ESP_ERROR_CHECK(
      gptimer_register_event_callbacks(s_state_timer, &callbacks, NULL));
  ESP_ERROR_CHECK(gptimer_enable(s_state_timer));
}

static void setup_blink_timer(void)
{
  gptimer_config_t config = {
      .clk_src = GPTIMER_CLK_SRC_DEFAULT,
      .direction = GPTIMER_COUNT_UP,
      .resolution_hz = TRAFFIC_LIGHT_TIMER_RESOLUTION_HZ,
  };
  ESP_ERROR_CHECK(gptimer_new_timer(&config, &s_blink_timer));

  gptimer_event_callbacks_t callbacks = {
      .on_alarm = on_blink_timer_alarm,
  };
  ESP_ERROR_CHECK(
      gptimer_register_event_callbacks(s_blink_timer, &callbacks, NULL));
  ESP_ERROR_CHECK(gptimer_enable(s_blink_timer));
}

static void setup_leds(void)
{
  gpio_reset_pin(TRAFFIC_LIGHT_RED_LED_GPIO);
  gpio_set_direction(TRAFFIC_LIGHT_RED_LED_GPIO, GPIO_MODE_OUTPUT);
  gpio_set_level(TRAFFIC_LIGHT_RED_LED_GPIO, 0);

  gpio_reset_pin(TRAFFIC_LIGHT_YELLOW_LED_GPIO);
  gpio_set_direction(TRAFFIC_LIGHT_YELLOW_LED_GPIO, GPIO_MODE_OUTPUT);
  gpio_set_level(TRAFFIC_LIGHT_YELLOW_LED_GPIO, 0);

  gpio_reset_pin(TRAFFIC_LIGHT_GREEN_LED_GPIO);
  gpio_set_direction(TRAFFIC_LIGHT_GREEN_LED_GPIO, GPIO_MODE_OUTPUT);
  gpio_set_level(TRAFFIC_LIGHT_GREEN_LED_GPIO, 0);
}

void traffic_light_init(void)
{
  setup_state_timer();
  setup_blink_timer();
  setup_leds();
}

void traffic_light_set_mode(traffic_light_mode_t new_mode)
{
  if (new_mode == s_current_mode)
  {
    return;
  }

  stop_state_timer();
  stop_blink_timer();

  s_current_mode = new_mode;

  switch (s_current_mode)
  {
  case TRAFFIC_LIGHT_MODE_NORMAL:
    enter_state(TRAFFIC_LIGHT_STATE_GREEN);
    break;

  case TRAFFIC_LIGHT_MODE_YELLOW_BLINKING:
    set_lights(false, true, false);
    start_blink_timer(
        MILLISECONDS_TO_TICKS(TRAFFIC_LIGHT_BLINK_INTERVAL_MS));
    break;

  case TRAFFIC_LIGHT_MODE_OFF:
    set_lights(false, false, false);
    break;
  }
}

void traffic_light_process(void)
{
  if (s_state_timer_alarm_pending)
  {
    s_state_timer_alarm_pending = false;
    handle_state_timer_alarm();
  }

  if (s_blink_timer_alarm_pending)
  {
    s_blink_timer_alarm_pending = false;
    handle_blink_timer_alarm();
  }
}
