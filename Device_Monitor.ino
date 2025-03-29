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

// LVGL touchscreen
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

void log_print(lv_log_level_t level, const char * buf) {
  LV_UNUSED(level);
  Serial.println(buf);
  Serial.flush();
}

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
  
  // Check for new devices
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

// Improved function to get device count with error handling
int getDeviceCount() {
  String params[][2] = {{}};
  String req[][2] = {{"NewHostNumberOfEntries", ""}};
  
  if (!connection.action("Hosts:1", "GetHostNumberOfEntries", params, 0, req, 1)) {
    Serial.println("Error in GetHostNumberOfEntries");
    return -1;
  }
  
  if (req[0][1].length() == 0) {
    Serial.println("Empty response for device count");
    return -1;
  }
  
  return req[0][1].toInt();
}

// Task for updating device list with improved stability
void updateDeviceListTask(void *pvParameters) {
  // Wait 2 seconds before first run to ensure stable connection
  vTaskDelay(2000 / portTICK_PERIOD_MS);
  
  while(1) {
    if (updateInProgress) {
      vTaskDelay(100 / portTICK_PERIOD_MS);
      continue;
    }
    
    // Check WiFi connection
    if (WiFiMulti.run() != WL_CONNECTED) {
      Serial.println("WiFi not connected, skipping update");
      updateInProgress = false;
      vTaskDelay(10000 / portTICK_PERIOD_MS);
      continue;
    }
    
    updateInProgress = true;
    
    std::vector<String> newDeviceList;
    int numDevices = 0;
    
    // Try to get device count (with retry mechanism)
    for (int retry = 0; retry < 3; retry++) {
      numDevices = getDeviceCount();
      if (numDevices >= 0) break; // Success
      vTaskDelay(500 / portTICK_PERIOD_MS); // Wait on error
      Serial.println("Retrying device count...");
    }
    
    if (numDevices < 0) {
      Serial.println("Error: Could not get device count");
      updateInProgress = false;
      vTaskDelay(30000 / portTICK_PERIOD_MS);
      continue;
    }

    // Get device list
    bool readSuccess = true;
    for (int i = 0; i < numDevices; i++) {
      String params[][2] = {{"NewIndex", String(i)}};  
      String req[][2] = {{"NewIPAddress", ""}, {"NewMACAddress", ""}, {"NewHostName", ""}, {"NewActive", ""}};
      
      if (!connection.action("Hosts:1", "GetGenericHostEntry", params, 1, req, 4)) {
        Serial.printf("Error reading device %d\n", i);
        readSuccess = false;
        break;
      }

      if (req[3][1] == "1") {
        String deviceInfo = req[2][1] + " (" + req[0][1] + ")";
        newDeviceList.push_back(deviceInfo);
      }
    }

    if (!readSuccess) {
      Serial.println("Error reading device list, skipping update");
      updateInProgress = false;
      vTaskDelay(30000 / portTICK_PERIOD_MS);
      continue;
    }

    // Compare with last list and print changes
    if (newDeviceList != lastDeviceList) {
      printDeviceChanges(lastDeviceList, newDeviceList);
      
      if (xSemaphoreTake(lvgl_mutex, portMAX_DELAY) == pdTRUE) {
        lv_obj_clean(device_list);
        
        static lv_style_t style_list_item;
        lv_style_init(&style_list_item);
        lv_style_set_border_width(&style_list_item, 4);
        lv_style_set_border_color(&style_list_item, lv_color_hex(0x50409A));
        lv_style_set_border_side(&style_list_item, LV_BORDER_SIDE_BOTTOM);
        lv_style_set_pad_all(&style_list_item, 5);
        lv_style_set_radius(&style_list_item, 5);
        lv_style_set_bg_color(&style_list_item, lv_color_hex(0x954EC2));
        lv_style_set_text_color(&style_list_item, lv_color_hex(0x000000));
        lv_style_set_text_font(&style_list_item, &lv_font_montserrat_14);

        for (const auto& deviceInfo : newDeviceList) {
          lv_obj_t * btn = lv_list_add_btn(device_list, LV_SYMBOL_WIFI, deviceInfo.c_str());
          lv_obj_remove_flag(btn, LV_OBJ_FLAG_CLICKABLE);
          lv_obj_add_style(btn, &style_list_item, LV_PART_MAIN);
        }

        lastDeviceList = newDeviceList;
        xSemaphoreGive(lvgl_mutex);
      }
    }
    
    updateInProgress = false;
    vTaskDelay(30000 / portTICK_PERIOD_MS); // Update every 30 seconds
  }
}

void lv_create_main_gui(void) {
  lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(0x313866), LV_PART_MAIN);

  static lv_style_t style_list_base;
  lv_style_init(&style_list_base);
  lv_style_set_border_width(&style_list_base, 0);
  lv_style_set_bg_color(&style_list_base, lv_color_hex(0x313866));
  lv_style_set_pad_row(&style_list_base, 10);
  lv_style_set_pad_left(&style_list_base, 15);
  lv_style_set_pad_right(&style_list_base, 15);
  lv_style_set_pad_top(&style_list_base, 5);
  lv_style_set_pad_bottom(&style_list_base, 10);
  lv_style_set_text_font(&style_list_base, &lv_font_montserrat_12);

  device_list = lv_list_create(lv_scr_act());
  lv_obj_set_size(device_list, 320, 240);
  lv_obj_center(device_list);
  lv_obj_align(device_list, LV_ALIGN_TOP_LEFT, 0, 0);
  lv_obj_add_style(device_list, &style_list_base, LV_PART_MAIN);
}

void setup() {
  String LVGL_Arduino = String("LVGL Library Version: ") + lv_version_major() + "." + lv_version_minor() + "." + lv_version_patch();
  Serial.begin(115200);
  Serial.println(LVGL_Arduino);

  // Create mutex for LVGL thread safety
  lvgl_mutex = xSemaphoreCreateMutex();
  
  // Initialize LVGL
  lv_init();
  lv_log_register_print_cb(log_print);

  // Initialize touchscreen
  touchscreenSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  touchscreen.begin(touchscreenSPI);
  touchscreen.setRotation(2);

  // Initialize display
  lv_display_t * disp;
  disp = lv_tft_espi_create(SCREEN_WIDTH, SCREEN_HEIGHT, draw_buf, sizeof(draw_buf));
  lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_90);
  
  // Register touchscreen
  lv_indev_t * indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, touchscreen_read);

  // Create GUI
  lv_create_main_gui();

  // Connect to Wi-Fi
  WiFiMulti.addAP(WIFI_SSID, WIFI_PASS);
  while (WiFiMulti.run() != WL_CONNECTED) {
    Serial.print(".");
    delay(500);
  }
  Serial.println("\nWiFi connected!");
  
  // Initialize TR-064 connection (removed the if check since init() is void)
  connection.init();
  Serial.println("TR-064 connection initialized.");

  // Create task for updating device list
  xTaskCreate(
    updateDeviceListTask,   // Task function
    "UpdateDevices",       // Name of task
    4096,                 // Stack size (bytes)
    NULL,                 // Parameter to pass
    1,                   // Task priority
    NULL                  // Task handle
  );

  // Initial update with error handling
  updateInProgress = true;
  std::vector<String> initialList;
  int numDevices = getDeviceCount();
  
  if (numDevices > 0) {
    for (int i = 0; i < numDevices; i++) {
      String params[][2] = {{"NewIndex", String(i)}};  
      String req[][2] = {{"NewIPAddress", ""}, {"NewMACAddress", ""}, {"NewHostName", ""}, {"NewActive", ""}};  
      if (connection.action("Hosts:1", "GetGenericHostEntry", params, 1, req, 4)) {
        if (req[3][1] == "1") {
          String deviceInfo = req[2][1] + " (" + req[0][1] + ")";
          initialList.push_back(deviceInfo);
        }
      }
    }

    // Print initial devices
    Serial.println("Initial connected devices:");
    for (const auto& device : initialList) {
      Serial.println(device);
    }

    if (xSemaphoreTake(lvgl_mutex, portMAX_DELAY) == pdTRUE) {
      static lv_style_t style_list_item;
      lv_style_init(&style_list_item);
      lv_style_set_border_width(&style_list_item, 4);
      lv_style_set_border_color(&style_list_item, lv_color_hex(0x50409A));
      lv_style_set_border_side(&style_list_item, LV_BORDER_SIDE_BOTTOM);
      lv_style_set_pad_all(&style_list_item, 5);
      lv_style_set_radius(&style_list_item, 5);
      lv_style_set_bg_color(&style_list_item, lv_color_hex(0x954EC2));
      lv_style_set_text_color(&style_list_item, lv_color_hex(0x000000));
      lv_style_set_text_font(&style_list_item, &lv_font_montserrat_14);

      for (const auto& deviceInfo : initialList) {
        lv_obj_t * btn = lv_list_add_btn(device_list, LV_SYMBOL_WIFI, deviceInfo.c_str());
        lv_obj_remove_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_style(btn, &style_list_item, LV_PART_MAIN);
      }

      lastDeviceList = initialList;
      xSemaphoreGive(lvgl_mutex);
    }
  } else {
    Serial.println("No devices found or error reading device list");
  }
  
  updateInProgress = false;
}

void loop() {
  if (xSemaphoreTake(lvgl_mutex, portMAX_DELAY) == pdTRUE) {
    lv_task_handler();
    lv_tick_inc(5);
    xSemaphoreGive(lvgl_mutex);
  }
  delay(5);
}
