#include <stdlib.h>
#include <FlexCAN.h>
#include <TeensyCANBase.h>

#include "line_find.h"
#include "boiler_find.h"
#include "doubly_linked_list.h"
#include "point_preprocess.h"
#include "datatypes.h"

/**
   Utility to get the amount of free RAM
   From SDFat
*/
extern "C" char* sbrk(int incr);
int FreeRam() {
  char top;
  return &top - reinterpret_cast<char*>(sbrk(0));
}

// Packet loading data
uint8_t current_packet[22];
uint8_t subpacket_idx;
bool start;
uint8_t last_idx;

// Avoiding as-fast-as-possible loops, which increase CAN utilization too much
long LOOP_TIME = 10000; // Microseconds
long TIMEOUT = 10000;
long lastLidarData;

// Current data
uint16_t * distances;
long lidar_speed;
doubly_linked_list_node<lidar_datapoint> * lidar_data_start;
doubly_linked_list_node<line> * line_data_start;
boiler_location boiler;

uint8_t calculation_idx;

void setup() {
  CAN_begin();
  Serial.begin(115200);
  Serial1.begin(115200);
  delay(1000); // wait for serial to load
  distances = new uint16_t[360];
  init_trig();
  start = false;
  lidar_data_start = NULL;
  line_data_start = NULL;
  calculation_idx = 0;
  lidar_speed = 0;
  boiler.delta_x = 0;
  boiler.delta_y = 0;
}

void try_load_next_bytes();
void packet_to_array();
void load_linked_list();

void writeLongs(uint32_t id, long value1, long value2) {
  byte * msg = new byte[8];

  for (int i = 0; i < 4; i++) {
    msg[i] = (value1 >> i * 8) & 0xFF;
  }
  for (int i = 0; i < 4; i++) {
    msg[i + 4] = (value2 >> i * 8) & 0xFF;
  }

  CAN_write(id, msg);

  delete msg;
}

void loop() {
  long loopStart = micros();

  try_load_next_bytes();

  if (subpacket_idx == 21 && lidar_speed > 180000) {
    if (calculation_idx == 0) {
      calculation_idx = 1; // Start calculation
    }
  }

  // CAN send
  writeLongs(0x600, boiler.delta_x, boiler.delta_y);
  writeLongs(0x607, 0, lidar_speed);

#ifdef TIME
  long timing_start = micros();
  if (calculation_idx != 0) {
    Serial.print("calculation ");
    Serial.println(calculation_idx);
    Serial.flush(); // If code crashes, print will make it
  }
#endif
  if (calculation_idx == 1) {
    interpolate(&distances[0]);
    calculation_idx++;
  }
  else if (calculation_idx == 2) {
    load_linked_list();
    if (lidar_data_start == NULL) {
      calculation_idx = 0;
    }
    else {
      calculation_idx++;
    }
  }
  else if (calculation_idx == 3) {
    blur_points(lidar_data_start);
    calculation_idx++;
  }
  else if (calculation_idx == 4) {
    blur_points(lidar_data_start);
    calculation_idx++;
  }
  else if (calculation_idx == 5) {
    blur_points(lidar_data_start);
    calculation_idx++;
  }
  else if (calculation_idx == 6) {
    add_cartesians(lidar_data_start);
    calculation_idx++;
  }
  else if (calculation_idx == 7) {
    line_data_start = get_lines(lidar_data_start);
    if (line_data_start == NULL) {
      calculation_idx = 10;
    }
    else {
      calculation_idx++;
    }
  }
  else if (calculation_idx == 8) {
    boiler = get_boiler(line_data_start);
    calculation_idx++;
  }
  else if (calculation_idx == 9) {
    line_list_cleanup(line_data_start);
    calculation_idx++;
  }
  else if (calculation_idx == 10) {
    lidar_datapoint_list_cleanup(lidar_data_start);
    calculation_idx = 0;
  }
#ifdef TIME
  if (calculation_idx != 0) {
    Serial.print("calculation finished: ");
    Serial.print(micros() - timing_start);
    Serial.print("\t");
    Serial.println(FreeRam());
  }
#endif

  // Logging
  if (Serial.available()) {
    char request = Serial.read();
    if (request == '0') {
      Serial.println(lidar_speed);
      Serial.print("#");
    }
    else if (request == '1') {
      for (uint16_t i = 0; i < 360; i++) {
        if (distances[i] != 0) {
          for (uint8_t j = 1; j < 3; j++) {
            if (i < pow(10, j)) Serial.print("0");
          }
          Serial.print(i);
          Serial.print(",");
          for (uint8_t j = 1; j < 4; j++) {
            if (distances[i] < pow(10, j)) Serial.print("0");
          }
          Serial.println(distances[i]);
          delayMicroseconds(2);
        }
      }
      Serial.print("#");
    }
    else if (request == '2' && line_data_start != NULL) {
      doubly_linked_list_node<line> * node = line_data_start->next;
      bool finished = false;

      while (node != line_data_start && !finished) {
        for (uint8_t j = 1; j < 3; j++) {
          if (node->data->start_x < pow(10, j)) Serial.print("0");
        }
        Serial.print(node->data->start_x);
        Serial.print(",");
        for (uint8_t j = 1; j < 3; j++) {
          if (node->data->start_y < pow(10, j)) Serial.print("0");
        }
        Serial.print(node->data->start_y);
        Serial.print(",");
        for (uint8_t j = 1; j < 3; j++) {
          if (node->data->end_x < pow(10, j)) Serial.print("0");
        }
        Serial.print(node->data->end_x);
        Serial.print(",");
        for (uint8_t j = 1; j < 3; j++) {
          if (node->data->end_y < pow(10, j)) Serial.print("0");
        }
        Serial.print(node->data->end_y);
        Serial.print(",");
        delayMicroseconds(2);
        if (node == line_data_start->prev) {
          finished = true;
        }
        node = node->next;
      }
      Serial.print("#");
    }
  }

  if (micros() - loopStart < LOOP_TIME) {
    delayMicroseconds(LOOP_TIME - (micros() - loopStart));
  }

  if (micros() - lastLidarData < TIMEOUT) {
    lidar_speed = 0;
    boiler.delta_x = 0;
    boiler.delta_y = 0;
  }
}

/**
   Attempt to load the next byte from the LIDAR Serial
   into the packet array
*/
void try_load_next_bytes() {
  while (Serial1.available()) {
    lastLidarData = micros();
    uint8_t b = Serial1.read();
    if (b == 0xFA && !start) {
      subpacket_idx = 0;
      memset(current_packet, 0, 22);
      current_packet[0] = 0xFA;
      start = true;
    }
    else if (start) {
      subpacket_idx++;
      current_packet[subpacket_idx] = b;
      if (subpacket_idx == 21) {
        start = false;
        packet_to_array();
      }
    }
  }
}

/**
   Update the distance array with the latest packet
*/
void packet_to_array() {
  uint8_t index = current_packet[1] - 0xA0;
  if (index != last_idx) {
    bool error = false;
    double rawLidarSpeed = ((double) ((current_packet[3] << 8) | current_packet[2])) / 64.0;
    lidar_speed = (long) (rawLidarSpeed * 1000.0);
    for (uint8_t i = 0; i < 4; i++) {
      uint8_t data_start = i * 4 + 4;
      uint16_t angle = index * 4 + i;
      if (angle > 359) {
        return;
      }
      error = (current_packet[data_start + 1] & 0x80) > 0;
      if (!error) {
        uint16_t distance = 0;
        distance = ((current_packet[data_start]) | (current_packet[data_start + 1] & 0x0F) << 8);
        distances[angle] = distance;
      }
      else {
        distances[angle] = 0;
      }
    }
    last_idx = index;
  }
}

/**
   Transfer the distance array into
*/
void load_linked_list() {
  doubly_linked_list_node<lidar_datapoint> * previous_node = NULL;
  for (int i = 0; i < 360; i++) {
    if (distances[i] != 0) {
      if (previous_node == NULL) {
        previous_node = new doubly_linked_list_node<lidar_datapoint>;
        previous_node->data = new lidar_datapoint;
        previous_node->data->theta = i;
        previous_node->data->radius = distances[i];
        previous_node->next = NULL;
        previous_node->prev = NULL;
        lidar_data_start = previous_node;
      }
      else {
        doubly_linked_list_node<lidar_datapoint> * node = new doubly_linked_list_node<lidar_datapoint>;
        node->data = new lidar_datapoint;
        node->data->theta = i;
        node->data->radius = distances[i];
        node->prev = previous_node;
        previous_node->next = node;
        previous_node = node;
      }
    }
  }

  if (previous_node != NULL) {
    previous_node->next = lidar_data_start;
    lidar_data_start->prev = previous_node;
  }
  else {
    lidar_data_start = NULL;
  }
}

