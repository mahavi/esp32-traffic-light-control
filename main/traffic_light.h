#ifndef TRAFFIC_LIGHT_H_
#define TRAFFIC_LIGHT_H_

typedef enum
{
  TRAFFIC_LIGHT_MODE_OFF,
  TRAFFIC_LIGHT_MODE_NORMAL,
  TRAFFIC_LIGHT_MODE_YELLOW_BLINKING,
  TRAFFIC_LIGHT_MODE_PEDESTRIAN_CONTROLLED
} traffic_light_mode_t;

void traffic_light_init(void);
void traffic_light_set_mode(traffic_light_mode_t new_mode);
void traffic_light_process(void);

#endif // TRAFFIC_LIGHT_H_