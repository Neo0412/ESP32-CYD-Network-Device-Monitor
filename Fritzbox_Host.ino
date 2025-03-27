#include <lvgl.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <tr064.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <vector>
#include <string>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// List of previously detected devices
std::vector<String> lastDeviceList;
bool updateInProgress = false;

// WiFi and TR-064 settings
#define WIFI_SSID ""
#define WIFI_PASS ""
#define TR_PORT 49000
#define TR_IP "192.168.178.1"
#define TR_USER ""
#define TR_PASS ""

WiFiMulti WiFiMulti;
TR064 connection(TR_PORT, TR_IP, TR_USER, TR_PASS);

// Display settings
#define SCREEN_WIDTH 240
#define SCREEN_HEIGHT 320
#define DRAW_BUF_SIZE (SCREEN_WIDTH * SCREEN_HEIGHT / 10 * (LV_COLOR_DEPTH / 8))
uint32_t draw_buf[DRAW_BUF_SIZE / 4];

// LVGL touchscreen settings
#define XPT2046_IRQ 36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK 25
#define XPT2046_CS 33

SPIClass touchscreenSPI = SPIClass(VSPI);
XPT2046_Touchscreen touchscreen(XPT2046_CS, XPT2046_IRQ);

// Touchscreen coordinates
int x, y, z;

// Mutex for thread-safe LVGL operations
SemaphoreHandle_t lvgl_mutex;

// Function to log messages
void log_print(lv_log_level_t level, const char * buf) {
  LV_UNUSED(level);
  Serial.println(buf);
  Serial.flush();
}

// Function to read touchscreen input
void touchscreen_read(lv_indev_t * indev, lv_indev_data_t * data) {
  if(touchscreen.tirqTouched() && touchscreen.touched()) {
    TS_Point p = touchscreen.getPoint();
    x = map(p.x, 200, 3700, 1, SCREEN_WIDTH);
    y = map(p.y, 240, 3800, 1, SCREEN_HEIGHT);
    z = p.z;

    data->state = LV_INDEV_STATE_PRESSED;
    data->point.x = x;
    data->point.y = y;
  }
  else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

lv_obj_t * device_list;

// Function to compare two device lists and print changes
void printDeviceChanges(const std::vector<String>& oldList, const std::vector<String>& newList) {
  // Check for removed devices
  for (const auto& oldDevice : oldList) {
    bool found = false;
    for (const auto& newDevice : newList) {
      if (oldDevice == newDevice) {
        found = true;
        break;
      }
    }
    if (!found) {
      Serial.print("Device disconnected: ");
      Serial.println(oldDevice);
    }
  }
  
  // Check for newly connected devices
  for (const auto& newDevice : newList) {
    bool found = false;
    for (const auto& oldDevice : oldList) {
      if (newDevice == oldDevice) {
        found = true;
        break;
      }
    }
    if (!found) {
      Serial.print("New device connected: ");
      Serial.println(newDevice);
    }
  }
}

// Task for updating device list
void updateDeviceListTask(void *pvParameters) {
  while(1) {
    if (updateInProgress) {
      vTaskDelay(100 / portTICK_PERIOD_MS);
      continue;
    }
    
    updateInProgress = true;
    
    std::vector<String> newDeviceList;
    int numDevices = getDeviceCount();
    
    for (int i = 0; i < numDevices; i++) {
      String params[][2] = {{"NewIndex", String(i)}};  
      String req[][2] = {{"NewIPAddress", ""}, {"NewMACAddress", ""}, {"NewHostName", ""}, {"NewActive", ""}};  
      connection.action("Hosts:1", "GetGenericHostEntry", params, 1, req, 4);

      if (req[3][1] == "1") {
        String deviceInfo = req[2][1] + " (" + req[0][1] + ")";
        newDeviceList.push_back(deviceInfo);
      }
    }

    // Compare with last list and print changes
    if (newDeviceList != lastDeviceList) {
      printDeviceChanges(lastDeviceList, newDeviceList);
      lastDeviceList = newDeviceList;
    }
    
    updateInProgress = false;
    vTaskDelay(30000 / portTICK_PERIOD_MS); // Update every 30 seconds
  }
}

// Function to retrieve device count
int getDeviceCount() {
  String params[][2] = {{}};
  String req[][2] = {{"NewHostNumberOfEntries", ""}};
  connection.action("Hosts:1", "GetHostNumberOfEntries", params, 0, req, 1);
  return req[0][1].toInt();
}

void setup() {
  Serial.begin(115200);
  
  // Create mutex for LVGL thread safety
  lvgl_mutex = xSemaphoreCreateMutex();
  
  // Initialize LVGL
  lv_init();
  lv_log_register_print_cb(log_print);

  // Initialize touchscreen
  touchscreenSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  touchscreen.begin(touchscreenSPI);
  touchscreen.setRotation(2);

  // Connect to Wi-Fi
  WiFiMulti.addAP(WIFI_SSID, WIFI_PASS);
  while (WiFiMulti.run() != WL_CONNECTED) {
    Serial.print(".");
    delay(500);
  }
  Serial.println("\nWiFi connected!");
  connection.init();
  Serial.println("TR-064 connection established.");

  // Create task for updating device list
  xTaskCreate(
    updateDeviceListTask,
    "UpdateDevices",
    4096,
    NULL,
    1,
    NULL
  );
}

void loop() {
  if (xSemaphoreTake(lvgl_mutex, portMAX_DELAY) == pdTRUE) {
    lv_task_handler();
    lv_tick_inc(5);
    xSemaphoreGive(lvgl_mutex);
  }
  delay(5);
}
