#include <Arduino.h>
#include <LiquidCrystal_I2C.h>
#include <Wire.h>
#include <time.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "credentials.h"

#define LCD_SDA 13
#define LCD_SCL 14
#define UP_BUTTON 32
#define DOWN_BUTTON 33
#define ACTION_BUTTON 15
#define PHOTORESISTOR 4

#define BUTTON_DEBOUNCE_DELAY 50

#define NTP_SERVER "pool.ntp.org"
#define GMT_OFFSET_SEC 0
#define DAYLIGHT_OFFSET_SEC 0
#define WIFI_TIMEOUT_MS 20000

class Timer
{
protected:
  const char *name;
  time_t lastTriggerTime;

public:
  Timer(const char *displayName,
        time_t initialTime = time(nullptr))
      : name(displayName), lastTriggerTime(initialTime) {}

  virtual int32_t timeSince(time_t currentTime) const
  {
    return difftime(currentTime, lastTriggerTime);
  }

  virtual void trigger(time_t triggerTime)
  {
    lastTriggerTime = triggerTime;
  }

  time_t getLastTriggerTime() const
  {
    return lastTriggerTime;
  }

  const char *getDisplayName() const
  {
    return name;
  }

  virtual bool handleButtonPress(time_t currentTime) = 0;

  // Add a virtual method to check if timer is pollable
  virtual bool isPollable() const { return false; }
  virtual bool checkPoll(time_t currentTime) { return false; }
};

class ButtonTimer : public Timer
{
public:
  ButtonTimer(const char *displayName,
              time_t initialTime = time(nullptr))
      : Timer(displayName, initialTime) {}

  bool handleButtonPress(time_t currentTime) override
  {
    trigger(currentTime);
    return true;
  }
};

class PollingTimer : public Timer
{
private:
  time_t lastPollTime;
  uint32_t pollingInterval;

public:
  PollingTimer(const char *displayName,
               uint32_t interval,
               time_t initialTime = time(nullptr))
      : Timer(displayName, initialTime),
        pollingInterval(interval),
        lastPollTime(initialTime) {}

  bool shouldPoll(time_t currentTime) const
  {
    return difftime(currentTime, lastPollTime) >= pollingInterval;
  }

  bool handleButtonPress(time_t currentTime) override
  {
    if (poll())
    {
      return true;
    }
    return false;
  }

  virtual bool poll()
  {
    bool success = pollImpl();
    if (success)
    {
      lastPollTime = time(nullptr);
    }
    return success;
  }

  // Override to identify as pollable
  bool isPollable() const override { return true; }

  // Override to check and perform polling if needed
  bool checkPoll(time_t currentTime) override
  {
    if (shouldPoll(currentTime))
    {
      return poll();
    }
    return false;
  }

protected:
  virtual bool pollImpl() = 0;
};

class GitHubPollingTimer : public PollingTimer
{
private:
  static const size_t MAX_USERNAME_LENGTH = 39;
  char githubUser[MAX_USERNAME_LENGTH + 1]; // +1 for null terminator
  static const uint32_t DEFAULT_POLL_INTERVAL = 300;
  WiFiClientSecure client;

public:
  GitHubPollingTimer(const char *displayName,
                     const char *username,
                     uint32_t pollInterval = DEFAULT_POLL_INTERVAL,
                     time_t initialTime = time(nullptr))
      : PollingTimer(displayName, pollInterval, initialTime)
  {
    Serial.println("Starting GitHub timer constructor");

    // Check username length before copying
    if (strlen(username) > MAX_USERNAME_LENGTH)
    {
      Serial.println("Error: GitHub username exceeds maximum length of 39 characters");
      throw std::invalid_argument("GitHub username too long");
    }

    strcpy(githubUser, username);
    client.setInsecure();

    if (poll())
    {
      Serial.println("Initial GitHub poll successful");
    }
  }

protected:
  bool pollImpl() override
  {
    Serial.println("Starting GitHub poll");
    if (WiFi.status() != WL_CONNECTED)
    {
      Serial.println("WiFi not connected");
      return false;
    }

    HTTPClient http;
    http.setTimeout(5000);

    char url[96];
    snprintf(url, sizeof(url), "https://api.github.com/users/%s/events", githubUser);
    Serial.printf("Polling URL: %s\n", url);

    if (!http.begin(client, url))
    {
      Serial.println("HTTP begin failed");
      return false;
    }

    http.addHeader("Accept", "application/vnd.github.v3+json");
    http.addHeader("User-Agent", "ESP32");

    int httpCode = http.GET();
    Serial.printf("HTTP Response code: %d\n", httpCode);
    bool success = false;

    if (httpCode == HTTP_CODE_OK)
    {
      String response = http.getString();

      JsonDocument filter;
      filter[0]["created_at"] = true;

      JsonDocument doc;
      DeserializationError error = deserializeJson(doc, response.c_str(), DeserializationOption::Filter(filter));

      if (error)
      {
        Serial.printf("JSON parse error: %s\n", error.c_str());
      }

      if (!error && doc[0]["created_at"])
      {
        const char *dateStr = doc[0]["created_at"];
        Serial.printf("Found date string: %s\n", dateStr);
        struct tm tm = {0};
        if (strptime(dateStr, "%Y-%m-%dT%H:%M:%S", &tm) != NULL)
        {
          time_t eventTime = mktime(&tm);
          time_t currentTime = time(nullptr);
          Serial.printf("Event time: %ld, Current time: %ld\n", (long)eventTime, (long)currentTime);
          trigger(eventTime);
          success = true;
        }
      }
    }

    http.end();
    return success;
  }
};

class BlueskyPollingTimer : public PollingTimer
{
private:
  static const size_t MAX_HANDLE_LENGTH = 253;
  char handle[MAX_HANDLE_LENGTH + 1]; // +1 for null terminator
  static const uint32_t DEFAULT_POLL_INTERVAL = 300;
  WiFiClientSecure client;

public:
  BlueskyPollingTimer(const char *displayName,
                      const char *userHandle,
                      uint32_t pollInterval = DEFAULT_POLL_INTERVAL,
                      time_t initialTime = time(nullptr))
      : PollingTimer(displayName, pollInterval, initialTime)
  {
    Serial.println("Starting Bluesky timer constructor");

    // Check handle length before copying
    if (strlen(handle) > MAX_HANDLE_LENGTH)
    {
      Serial.println("Error: Bluesky handle exceeds maximum length of 253 characters");
      throw std::invalid_argument("Bluesky handle too long");
    }

    strcpy(handle, userHandle);
    handle[sizeof(handle) - 1] = '\0';

    client.setInsecure();

    if (poll())
    {
      Serial.println("Initial Bluesky poll successful");
    }
  }

protected:
  bool pollImpl() override
  {
    Serial.println("Starting Bluesky poll");
    if (WiFi.status() != WL_CONNECTED)
    {
      Serial.println("WiFi not connected");
      return false;
    }

    HTTPClient http;
    http.setTimeout(5000);

    // Using Bluesky API endpoint
    char url[128];
    snprintf(url, sizeof(url), "https://bsky.social/xrpc/com.atproto.repo.listRecords?repo=%s&collection=app.bsky.feed.post", handle);
    Serial.printf("Polling URL: %s\n", url);

    if (!http.begin(client, url))
    {
      Serial.println("HTTP begin failed");
      return false;
    }

    int httpCode = http.GET();
    Serial.printf("HTTP Response code: %d\n", httpCode);
    bool success = false;

    if (httpCode == HTTP_CODE_OK)
    {
      String response = http.getString();

      JsonDocument filter;
      filter["records"][0]["value"]["createdAt"] = true;

      JsonDocument doc;
      DeserializationError error = deserializeJson(doc, response.c_str(), DeserializationOption::Filter(filter));

      if (error)
      {
        Serial.printf("JSON parse error: %s\n", error.c_str());
      }

      if (!error && doc["records"][0]["value"]["createdAt"])
      {
        const char *dateStr = doc["records"][0]["value"]["createdAt"];
        Serial.printf("Found date string: %s\n", dateStr);
        struct tm tm = {0};
        if (strptime(dateStr, "%Y-%m-%dT%H:%M:%S", &tm) != NULL)
        {
          time_t eventTime = mktime(&tm);
          trigger(eventTime);
          success = true;
        }
      }
    }

    http.end();
    return success;
  }
};

class WeatherPollingTimer : public PollingTimer
{
private:
  float latitude;
  float longitude;
  float currentTemp;
  char displayName[32];                              // Store the full display name here
  static const uint32_t DEFAULT_POLL_INTERVAL = 900; // 15 minutes
  WiFiClientSecure client;

  time_t findLastAboveZero()
  {
    time_t now = time(nullptr);
    time_t startTime = now - (30 * 24 * 60 * 60); // Get 30 days of history
    char url[256];

    // Format the dates for Open-Meteo
    struct tm timeinfo;
    char startDate[11], endDate[11];
    gmtime_r(&startTime, &timeinfo);
    strftime(startDate, sizeof(startDate), "%Y-%m-%d", &timeinfo);
    gmtime_r(&now, &timeinfo);
    strftime(endDate, sizeof(endDate), "%Y-%m-%d", &timeinfo);

    snprintf(url, sizeof(url),
             "https://archive-api.open-meteo.com/v1/archive?"
             "latitude=%.4f&longitude=%.4f&start_date=%s&end_date=%s"
             "&hourly=temperature_2m",
             latitude, longitude, startDate, endDate);

    HTTPClient http;
    if (!http.begin(client, url))
      return now;

    int httpCode = http.GET();
    time_t lastAboveZero = now;

    if (httpCode == HTTP_CODE_OK)
    {
      String response = http.getString();
      JsonDocument doc;
      DeserializationError error = deserializeJson(doc, response);

      if (!error && doc["hourly"]["time"].is<JsonArray>() &&
          doc["hourly"]["temperature_2m"].is<JsonArray>())
      {
        JsonArray times = doc["hourly"]["time"];
        JsonArray temps = doc["hourly"]["temperature_2m"];

        bool foundAboveZero = false;
        // Search from most recent to oldest
        for (int i = times.size() - 1; i >= 0; i--)
        {
          float temp = temps[i];
          const char *timeStr = times[i];

          if (temp > 0.0f)
          {
            // Convert ISO time string to epoch
            struct tm tm = {};
            strptime(timeStr, "%Y-%m-%dT%H:%M", &tm);
            lastAboveZero = mktime(&tm);
            foundAboveZero = true;
            break;
          }
        }

        if (!foundAboveZero)
        {
          lastAboveZero = startTime;
        }
      }
    }

    http.end();
    return lastAboveZero;
  }

public:
  WeatherPollingTimer(const char *displayName,
                      float lat, float lon,
                      uint32_t pollInterval = DEFAULT_POLL_INTERVAL)
      : PollingTimer(displayName, pollInterval, time(nullptr)),
        latitude(lat), longitude(lon), currentTemp(0.0f)
  {
    client.setInsecure();
    time_t lastAboveZero = findLastAboveZero();
    trigger(lastAboveZero);
  }

protected:
  bool pollImpl() override
  {
    Serial.println("Starting Weather poll");
    if (WiFi.status() != WL_CONNECTED)
    {
      Serial.println("WiFi not connected");
      return false;
    }

    HTTPClient http;
    char url[128];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast?"
             "latitude=%.4f&longitude=%.4f"
             "&current=temperature_2m",
             latitude, longitude);

    Serial.printf("Polling URL: %s\n", url);

    if (!http.begin(client, url))
    {
      Serial.println("HTTP begin failed");
      return false;
    }

    int httpCode = http.GET();
    Serial.printf("HTTP Response code: %d\n", httpCode);
    bool success = false;

    if (httpCode == HTTP_CODE_OK)
    {
      String response = http.getString();
      Serial.printf("Response: %s\n", response.c_str());

      JsonDocument doc;
      DeserializationError error = deserializeJson(doc, response);

      if (error)
      {
        Serial.printf("JSON parse error: %s\n", error.c_str());
      }

      if (!error && doc["current"]["temperature_2m"].is<float>())
      {
        currentTemp = doc["current"]["temperature_2m"];
        Serial.printf("Current temperature: %.1f°C\n", currentTemp);

        if (currentTemp > 0.0f)
        {
          Serial.println("Temperature above 0°C, updating trigger time");
          trigger(time(nullptr));
        }
        success = true;
      }
    }

    http.end();
    return success;
  }
};

// Class to manage timer display and button interaction
class TimerDisplay
{
private:
  Timer **timers;
  uint8_t timerCount;
  uint8_t currentIndex;
  uint8_t actionButtonPin;
  LiquidCrystal_I2C &lcd;
  String lastName;
  int32_t lastSeconds;

public:
  TimerDisplay(Timer **timerArray, uint8_t count, uint8_t buttonPin, LiquidCrystal_I2C &lcdDisplay)
      : timers(timerArray), timerCount(count), currentIndex(0),
        actionButtonPin(buttonPin), lcd(lcdDisplay),
        lastName(""), lastSeconds(-1) {}

  void update(time_t now)
  {
    // Check if current timer needs polling
    if (Timer *current = getCurrentTimer())
    {
      if (current->isPollable())
      {
        current->checkPoll(now);
      }
    }

    checkButton(now);
    updateDisplay(now);
    checkNavigationButtons();
  }

  // Check button and trigger current timer if pressed
  void checkButton(time_t currentTime)
  {
    static bool lastButtonState = HIGH;
    bool currentButtonState = digitalRead(actionButtonPin);

    if (currentButtonState == LOW && lastButtonState == HIGH)
    {
      // Clear timestamp so no characters linger
      lcd.setCursor(0, 1);
      lcd.print("                ");

      timers[currentIndex]->handleButtonPress(currentTime);
    }

    lastButtonState = currentButtonState;

    delay(BUTTON_DEBOUNCE_DELAY); // Simple debounce
  }

  void nextTimer()
  {
    currentIndex = (currentIndex + 1) % timerCount;
  }

  void previousTimer()
  {
    currentIndex = (currentIndex - 1) % timerCount;
  }

  // Get currently selected timer
  Timer *getCurrentTimer() const
  {
    return timers[currentIndex];
  }

private:
  void updateDisplay(time_t now)
  {
    Timer *current = getCurrentTimer();
    String name = current->getDisplayName();
    int32_t seconds = current->timeSince(now);

    if (name != lastName || seconds != lastSeconds)
    {
      if (name != lastName)
      {
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print(name);
        lastName = name;
      }

      int hours = seconds / 3600;
      int minutes = (seconds % 3600) / 60;
      int secs = seconds % 60;

      char timeStr[16];
      sprintf(timeStr, "%02d:%02d:%02d", hours, minutes, secs);

      int timeStrLen = strlen(timeStr);
      int startPos = max(0, 16 - timeStrLen);

      lcd.setCursor(startPos, 1);
      lcd.print(timeStr);

      lastSeconds = seconds;
    }
  }

  void checkNavigationButtons()
  {
    if (digitalRead(DOWN_BUTTON) == LOW)
    {
      nextTimer();
      delay(BUTTON_DEBOUNCE_DELAY);
    }

    if (digitalRead(UP_BUTTON) == LOW)
    {
      previousTimer();
      delay(BUTTON_DEBOUNCE_DELAY);
    }
  }
};

void initTime()
{
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);

  time_t now = 0;
  while (now < 24 * 3600)
  {
    delay(500);
    now = time(nullptr);
  }
}

// PCF8574T, IIC address is 0x27, PCF8574AT is 0x3F.
LiquidCrystal_I2C lcd(0x27, 16, 2);

// Create array of timer pointers. Adjust size as needed
Timer *timerArray[4];

// Create display controller
TimerDisplay *timerDisplay;

void connectToWifi()
{
  Serial.print("Connecting to WiFi");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long startAttemptTime = millis();

  // Keep looping while we're not connected and haven't reached the timeout
  while (WiFi.status() != WL_CONNECTED &&
         millis() - startAttemptTime < WIFI_TIMEOUT_MS)
  {
    Serial.print(".");
    delay(100);
  }

  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.println("\nFailed to connect to WiFi!");
    return;
  }

  Serial.println("\nConnected!");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());
}

void setup()
{
  Wire.begin(LCD_SDA, LCD_SCL);
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Initializing...");

  Serial.begin(115200);

  pinMode(UP_BUTTON, INPUT_PULLUP);
  pinMode(DOWN_BUTTON, INPUT_PULLUP);
  pinMode(ACTION_BUTTON, INPUT_PULLUP);

  connectToWifi();
  initTime();

  time_t now = time(nullptr);

  Serial.println("Starting timer initialization");
  timerArray[0] = new ButtonTimer("Last drank water", now);
  timerArray[1] = new GitHubPollingTimer("Last GitHub push", "evjrob", 300, now);
  timerArray[2] = new BlueskyPollingTimer("Last Bsky post", "evjrob.bsky.social", 300, now);
  timerArray[3] = new WeatherPollingTimer("Last above 0"
                                          "\xDF"
                                          "C",
                                          49.8954f, -97.1385f, 900);

  timerDisplay = new TimerDisplay(timerArray, 4, ACTION_BUTTON, lcd);
}

void loop()
{
  time_t now = time(nullptr);
  timerDisplay->update(now);
}