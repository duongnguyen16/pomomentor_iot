#include "config.h"
#include <Wire.h>

#include <LiquidCrystal_I2C.h>
#include <VL53L0X.h>
#include <WiFi.h>

#include <WebSocketsServer.h>
#include <time.h>
#include <ArduinoJson.h>
#include <EEPROM.h>
#include "CharacterHelper.h"
#include <WiFiManager.h>

LiquidCrystal_I2C lcd(0x27, 16, 2);

VL53L0X sensor;

#define SDA_PIN 21
#define SCL_PIN 22
#define XSHUT_PIN 4

#define BTN_PIN 32
#define BUZZER_PIN 15

#define S1_PIN 16
#define S2_PIN 17
#define S3_PIN 18
#define S4_PIN 19

enum UIMode
{
    CLOCK_MODE,
    POMODORO_MODE,
    WIFI_MODE
};

UIMode currentUIMode = CLOCK_MODE;

const char *ntpServer = "pool.ntp.org";

const char *AP_NAME = "PomoMentor_Setup";
const char *AP_PASSWORD = "pomosetup";
const int WIFI_CONFIG_TIMEOUT = 180;
const int WIFI_CONNECT_TIMEOUT = 20;
bool portalActive = false;

WebSocketsServer webSocket(81);

int lastReportedDistance = -1;
unsigned long lastDistanceReadTime = 0;
const unsigned long DISTANCE_READ_INTERVAL = 500;

enum PomodoroState
{
    IDLE,
    RUNNING_WORK,
    PAUSED_WORK,
    RUNNING_SHORT_BREAK,
    PAUSED_SHORT_BREAK,
    RUNNING_LONG_BREAK,
    PAUSED_LONG_BREAK,
    FINISHED
};

PomodoroState currentPomodoroState = IDLE;
unsigned long pomodoroStartTime = 0;
unsigned long pomodoroPauseStartTime = 0;
unsigned long pomodoroTargetTime = 0;
int currentPomodoroDuration = 0;
int pomodoroWorkSessionCount = 0;
unsigned long finishDisplayEndTime = 0;

const int DEFAULT_WORK_DURATION_SEC = 25 * 60;
const int DEFAULT_SHORT_BREAK_DURATION_SEC = 5 * 60;
const int DEFAULT_LONG_BREAK_DURATION_SEC = 15 * 60;
const int WORK_SESSIONS_BEFORE_LONG_BREAK = 4;

String cachedLcdLine1 = "";
String cachedLcdLine2 = "";
unsigned long lastDisplayUpdateTime = 0;

#define MAX_TASKS 20
#define MAX_TASK_NAME_LENGTH 50
struct Task
{
    char id[16];
    char title[MAX_TASK_NAME_LENGTH];
    bool completed;
    bool active;
};
Task tasks[MAX_TASKS];
int taskCount = 0;
char activeTaskId[16] = "";

struct tm timeinfo;
time_t now;
unsigned long lastClockUpdateTime = 0;
const unsigned long CLOCK_UPDATE_INTERVAL = 1000;

#define EEPROM_SIZE 512
#define SETTINGS_VERSION 1
#define SETTINGS_ADDR 0

struct Settings
{
    int version;
    int workDuration;
    int shortBreakDuration;
    int longBreakDuration;
    int workSessionsBeforeLongBreak;
    int timezone;
    bool focusEnabled;
    int distanceCalibration;
    char reserved[64];
};

Settings deviceSettings = {
    SETTINGS_VERSION,
    25 * 60,
    5 * 60,
    15 * 60,
    4,
    7,
    false,
    50,
    ""};

void loadSettings()
{
    Serial.println("Loading settings from EEPROM...");
    EEPROM.begin(EEPROM_SIZE);

    int savedVersion;
    EEPROM.get(SETTINGS_ADDR, savedVersion);

    if (savedVersion == SETTINGS_VERSION)
    {
        EEPROM.get(SETTINGS_ADDR, deviceSettings);
        Serial.println("Settings loaded successfully");
        Serial.println("Work duration: " + String(deviceSettings.workDuration / 60) + " minutes");
        Serial.println("Short break: " + String(deviceSettings.shortBreakDuration / 60) + " minutes");
        Serial.println("Long break: " + String(deviceSettings.longBreakDuration / 60) + " minutes");
        Serial.println("Sessions before long break: " + String(deviceSettings.workSessionsBeforeLongBreak));
    }
    else
    {
        Serial.println("Settings version mismatch or first run, using defaults");
        saveSettings();
    }
}

void saveSettings()
{
    Serial.println("Saving settings to EEPROM...");
    EEPROM.begin(EEPROM_SIZE);
    EEPROM.put(SETTINGS_ADDR, deviceSettings);
    EEPROM.commit();
    Serial.println("Settings saved");
}

void getSettingsAsJson(String &jsonOutput)
{
    DynamicJsonDocument doc(256);

    doc["work"] = deviceSettings.workDuration / 60;
    doc["shortBreak"] = deviceSettings.shortBreakDuration / 60;
    doc["longBreak"] = deviceSettings.longBreakDuration / 60;
    doc["longBreakAfter"] = deviceSettings.workSessionsBeforeLongBreak;
    doc["timezone"] = deviceSettings.timezone;
    doc["focusEnabled"] = deviceSettings.focusEnabled ? 1 : 0;
    doc["distanceCal"] = deviceSettings.distanceCalibration;

    serializeJson(doc, jsonOutput);
    Serial.print("Settings JSON: ");
    Serial.println(jsonOutput);
}

bool updateSettingsFromJson(const char *jsonString)
{

    char jsonCopy[256];
    strncpy(jsonCopy, jsonString, 255);
    jsonCopy[255] = '\0';

    DynamicJsonDocument doc(256);
    DeserializationError error = deserializeJson(doc, jsonCopy);

    if (error)
    {
        Serial.print("Failed to parse settings JSON: ");
        Serial.println(error.c_str());
        Serial.print("Received JSON string: '");
        Serial.print(jsonCopy);
        Serial.println("'");
        return false;
    }

    Serial.print("Received settings JSON: ");
    Serial.println(jsonCopy);

    bool changed = false;

    if (doc.containsKey("work"))
    {
        int newVal = doc["work"];
        if (newVal > 0 && newVal <= 60 && deviceSettings.workDuration != newVal * 60)
        {
            deviceSettings.workDuration = newVal * 60;
            Serial.println("Updated work duration to " + String(newVal) + " minutes");
            changed = true;
        }
    }

    if (doc.containsKey("shortBreak"))
    {
        int newVal = doc["shortBreak"];
        if (newVal > 0 && newVal <= 30 && deviceSettings.shortBreakDuration != newVal * 60)
        {
            deviceSettings.shortBreakDuration = newVal * 60;
            Serial.println("Updated short break duration to " + String(newVal) + " minutes");
            changed = true;
        }
    }

    if (doc.containsKey("longBreak"))
    {
        int newVal = doc["longBreak"];
        if (newVal > 0 && newVal <= 60 && deviceSettings.longBreakDuration != newVal * 60)
        {
            deviceSettings.longBreakDuration = newVal * 60;
            Serial.println("Updated long break duration to " + String(newVal) + " minutes");
            changed = true;
        }
    }

    if (doc.containsKey("longBreakAfter"))
    {
        int newVal = doc["longBreakAfter"];
        if (newVal > 0 && newVal <= 10 && deviceSettings.workSessionsBeforeLongBreak != newVal)
        {
            deviceSettings.workSessionsBeforeLongBreak = newVal;
            Serial.println("Updated sessions before long break to " + String(newVal));
            changed = true;
        }
    }

    if (doc.containsKey("timezone"))
    {
        int newVal = doc["timezone"];
        if (newVal >= -12 && newVal <= 14 && deviceSettings.timezone != newVal)
        {
            deviceSettings.timezone = newVal;
            Serial.println("Updated timezone to UTC" + String(newVal >= 0 ? "+" : "") + String(newVal));
            changed = true;
        }
    }

    if (doc.containsKey("focusEnabled"))
    {
        bool newVal = doc["focusEnabled"] ? true : false;
        if (deviceSettings.focusEnabled != newVal)
        {
            deviceSettings.focusEnabled = newVal;
            Serial.println("Updated focus mode to " + String(newVal ? "enabled" : "disabled"));
            changed = true;
        }
    }

    if (doc.containsKey("distanceCal"))
    {
        int newVal = doc["distanceCal"];
        if (newVal >= 5 && newVal <= 200 && deviceSettings.distanceCalibration != newVal)
        {
            deviceSettings.distanceCalibration = newVal;
            Serial.println("Updated distance calibration to " + String(newVal) + " mm");
            changed = true;
        }
    }

    if (changed)
    {
        saveSettings();
        return true;
    }
    else
    {
        Serial.println("No settings changed or invalid settings received");
        return false;
    }
}

void updateDisplay();
void getPomodoroStateAsJson(String &jsonOutput);
void broadcastPomodoroState();
void startPomodoro(PomodoroState type, int durationSeconds);
void pausePomodoro();
void resumePomodoro();
void stopPomodoro();
void updatePomodoro();
void playTone(const char *toneType);
void handlePomodoroCommand(String command);
void checkPhysicalButton();
void updateVL53L0X();
void webSocketEvent(uint8_t num, WStype_t type, uint8_t *payload, size_t length);
void sendFullState(uint8_t clientId);
void getTasksAsJson(String &jsonOutput);
bool addTask(const char *title);
bool deleteTask(const char *id);
bool toggleTaskCompletion(const char *id);
bool setActiveTask(const char *id);
bool updateTaskTitle(const char *id, const char *newTitle);
int findTaskById(const char *id);
void getSettingsAsJson(String &jsonOutput);
bool updateSettingsFromJson(const char *jsonString);
bool setupWifi(bool forceConfig = false);
void handleWiFiReset();
void saveLCDBackup();
void restoreLCDBackup();

bool focusModeActivePause = false;
bool focusAlertSounded = false;
unsigned long lastBlinkToggleTime = 0;
bool blinkState = true;
const unsigned long BLINK_INTERVAL = 500;

String backupLcdLine1 = "";
String backupLcdLine2 = "";
bool lcdBackupSaved = false;

void showBootStatus(const String &statusMessage)
{
    updateLcdRaw("PomoMentor", statusMessage);
    cachedLcdLine1 = "PomoMentor";
    cachedLcdLine2 = statusMessage;
    Serial.print("Boot status: ");
    Serial.println(statusMessage);
}

void generateTaskId(char *buffer)
{
    unsigned long timestamp = millis();
    int randomNum = random(1000, 9999);
    sprintf(buffer, "%lu%d", timestamp, randomNum);
}

bool addTask(const char *title)
{
    if (taskCount >= MAX_TASKS)
        return false;
    Task &task = tasks[taskCount];
    generateTaskId(task.id);
    strncpy(task.title, title, MAX_TASK_NAME_LENGTH - 1);
    task.title[MAX_TASK_NAME_LENGTH - 1] = '\0';
    task.completed = false;
    task.active = false;
    taskCount++;
    Serial.print("Task added: ");
    Serial.println(title);
    return true;
}

int findTaskById(const char *id)
{
    for (int i = 0; i < taskCount; i++)
    {
        if (strcmp(tasks[i].id, id) == 0)
            return i;
    }
    return -1;
}

bool updateTaskTitle(const char *id, const char *newTitle)
{
    int index = findTaskById(id);
    if (index == -1)
        return false;
    strncpy(tasks[index].title, newTitle, MAX_TASK_NAME_LENGTH - 1);
    tasks[index].title[MAX_TASK_NAME_LENGTH - 1] = '\0';
    Serial.print("Task updated: ");
    Serial.println(newTitle);
    return true;
}

bool toggleTaskCompletion(const char *id)
{
    int index = findTaskById(id);
    if (index == -1)
        return false;
    tasks[index].completed = !tasks[index].completed;
    if (tasks[index].completed && tasks[index].active)
        setActiveTask("");
    Serial.print("Task toggled: ");
    Serial.println(tasks[index].title);
    return true;
}

bool setActiveTask(const char *id)
{
    for (int i = 0; i < taskCount; i++)
        tasks[i].active = false;
    if (strlen(id) == 0)
    {
        strcpy(activeTaskId, "");
        Serial.println("Active task cleared");
        return true;
    }
    int index = findTaskById(id);
    if (index == -1 || tasks[index].completed)
        return false;
    tasks[index].active = true;
    strcpy(activeTaskId, id);
    Serial.print("Active task set: ");
    Serial.println(tasks[index].title);
    return true;
}

bool deleteTask(const char *id)
{
    int index = findTaskById(id);
    if (index == -1)
        return false;
    Serial.print("Task deleted: ");
    Serial.println(tasks[index].title);
    if (tasks[index].active)
        strcpy(activeTaskId, "");
    for (int i = index; i < taskCount - 1; i++)
    {
        memcpy(&tasks[i], &tasks[i + 1], sizeof(Task));
    }
    taskCount--;
    return true;
}

void getTasksAsJson(String &jsonOutput)
{
    DynamicJsonDocument doc(JSON_ARRAY_SIZE(MAX_TASKS) + taskCount * JSON_OBJECT_SIZE(4) + taskCount * MAX_TASK_NAME_LENGTH);
    JsonArray tasksArray = doc.to<JsonArray>();
    for (int i = 0; i < taskCount; i++)
    {
        JsonObject taskObj = tasksArray.createNestedObject();
        taskObj["id"] = tasks[i].id;
        taskObj["title"] = tasks[i].title;
        taskObj["completed"] = tasks[i].completed;
        taskObj["active"] = tasks[i].active;
    }
    serializeJson(doc, jsonOutput);
}

void scanI2C()
{
    byte error, address;
    int nDevices = 0;
    Serial.println("Scanning for I2C devices...");
    for (address = 1; address < 127; address++)
    {
        Wire.beginTransmission(address);
        error = Wire.endTransmission();
        if (error == 0)
        {
            Serial.print("I2C device found at address 0x");
            if (address < 16)
                Serial.print("0");
            Serial.print(address, HEX);
            if (address == 0x29)
                Serial.print(" (VL53L0X)");
            else if (address == 0x27 || address == 0x3F)
                Serial.print(" (LCD Display)");
            Serial.println();
            nDevices++;
        }
    }
    if (nDevices == 0)
        Serial.println("No I2C devices found");
    else
        Serial.println("I2C scan complete");
}

void updateLcdRaw(const String &line1, const String &line2)
{
    String paddedLine1 = line1;
    String paddedLine2 = line2;

    if (paddedLine1.length() > 16)
        paddedLine1 = paddedLine1.substring(0, 16);
    if (paddedLine2.length() > 16)
        paddedLine2 = paddedLine2.substring(0, 16);

    while (paddedLine1.length() < 16)
        paddedLine1 += " ";
    while (paddedLine2.length() < 16)
        paddedLine2 += " ";

    if (paddedLine1 != cachedLcdLine1 || paddedLine2 != cachedLcdLine2)
    {

        lcd.setCursor(0, 0);
        lcd.print(paddedLine1);
        lcd.setCursor(0, 1);
        lcd.print(paddedLine2);
        cachedLcdLine1 = paddedLine1;
        cachedLcdLine2 = paddedLine2;
        lastDisplayUpdateTime = millis();
    }
}

void playTone(const char *toneType)
{
#ifdef BUZZER_PIN
    pinMode(BUZZER_PIN, OUTPUT);

    digitalWrite(BUZZER_PIN, LOW);

    if (strcmp(toneType, "startup") == 0)
    {

        digitalWrite(BUZZER_PIN, HIGH);
        delay(100);
        digitalWrite(BUZZER_PIN, LOW);
    }
    else if (strcmp(toneType, "start") == 0)
    {

        digitalWrite(BUZZER_PIN, HIGH);
        delay(100);
        digitalWrite(BUZZER_PIN, LOW);
        delay(100);
        digitalWrite(BUZZER_PIN, HIGH);
        delay(100);
        digitalWrite(BUZZER_PIN, LOW);
    }
    else if (strcmp(toneType, "stop") == 0)
    {

        digitalWrite(BUZZER_PIN, HIGH);
        delay(100);
        digitalWrite(BUZZER_PIN, LOW);
    }
    else if (strcmp(toneType, "finish") == 0)
    {

        digitalWrite(BUZZER_PIN, HIGH);
        delay(300);
        digitalWrite(BUZZER_PIN, LOW);
        delay(100);
        digitalWrite(BUZZER_PIN, HIGH);
        delay(100);
        digitalWrite(BUZZER_PIN, LOW);
    }
    else if (strcmp(toneType, "button") == 0)
    {

        digitalWrite(BUZZER_PIN, HIGH);
        delay(50);
        digitalWrite(BUZZER_PIN, LOW);
    }
    else if (strcmp(toneType, "longpress") == 0)
    {

        digitalWrite(BUZZER_PIN, HIGH);
        delay(150);
        digitalWrite(BUZZER_PIN, LOW);
    }

    digitalWrite(BUZZER_PIN, LOW);
#else

    Serial.print("TONE: ");
    Serial.println(toneType);
#endif
}

void startPomodoro(PomodoroState type, int durationSeconds)
{
    Serial.print("Starting Pomodoro: Type=");
    Serial.print(type);
    Serial.print(", Duration=");
    Serial.println(durationSeconds);
    currentPomodoroState = type;
    currentPomodoroDuration = durationSeconds;
    pomodoroStartTime = millis();
    pomodoroTargetTime = pomodoroStartTime + (unsigned long)durationSeconds * 1000;
    finishDisplayEndTime = 0;

    currentUIMode = POMODORO_MODE;

    playTone("start");

    cachedLcdLine1 = "";
    cachedLcdLine2 = "";

    updateDisplay();
    broadcastPomodoroState();
}

void pausePomodoro()
{
    if (currentPomodoroState == RUNNING_WORK || currentPomodoroState == RUNNING_SHORT_BREAK || currentPomodoroState == RUNNING_LONG_BREAK)
    {
        Serial.println("Pausing Pomodoro");
        pomodoroPauseStartTime = millis();

        if (!focusModeActivePause)
        {
            playTone("stop");
        }

        if (currentPomodoroState == RUNNING_WORK)
            currentPomodoroState = PAUSED_WORK;
        else if (currentPomodoroState == RUNNING_SHORT_BREAK)
            currentPomodoroState = PAUSED_SHORT_BREAK;
        else if (currentPomodoroState == RUNNING_LONG_BREAK)
            currentPomodoroState = PAUSED_LONG_BREAK;

        cachedLcdLine1 = "";
        cachedLcdLine2 = "";
        updateDisplay();
        broadcastPomodoroState();
    }
}

void resumePomodoro()
{
    if (currentPomodoroState == PAUSED_WORK || currentPomodoroState == PAUSED_SHORT_BREAK || currentPomodoroState == PAUSED_LONG_BREAK)
    {
        Serial.println("Resuming Pomodoro");

        if (!focusModeActivePause)
        {
            playTone("start");
        }

        unsigned long pauseDuration = millis() - pomodoroPauseStartTime;
        pomodoroTargetTime += pauseDuration;
        pomodoroStartTime += pauseDuration;

        if (currentPomodoroState == PAUSED_WORK)
            currentPomodoroState = RUNNING_WORK;
        else if (currentPomodoroState == PAUSED_SHORT_BREAK)
            currentPomodoroState = RUNNING_SHORT_BREAK;
        else if (currentPomodoroState == PAUSED_LONG_BREAK)
            currentPomodoroState = RUNNING_LONG_BREAK;

        cachedLcdLine1 = "";
        cachedLcdLine2 = "";
        updateDisplay();
        broadcastPomodoroState();
    }
}

void stopPomodoro()
{
    if (currentPomodoroState != IDLE)
    {
        Serial.println("Stopping Pomodoro");

        playTone("stop");

        focusModeActivePause = false;
        focusAlertSounded = false;

        currentPomodoroState = IDLE;
        finishDisplayEndTime = 0;
        cachedLcdLine1 = "";
        cachedLcdLine2 = "";
        updateDisplay();
        broadcastPomodoroState();
    }
}

void updatePomodoro()
{
    unsigned long now = millis();

    if (currentPomodoroState == FINISHED)
    {
        if (now >= finishDisplayEndTime)
        {
            Serial.println("Finish display timeout. Determining next state.");

            PomodoroState lastStateBeforeFinish = IDLE;
            stopPomodoro();
        }

        return;
    }

    if (currentPomodoroState == RUNNING_WORK || currentPomodoroState == RUNNING_SHORT_BREAK || currentPomodoroState == RUNNING_LONG_BREAK)
    {
        if (now >= pomodoroTargetTime)
        {

            PomodoroState finishedState = currentPomodoroState;
            Serial.print("Pomodoro Finished: ");
            Serial.println(finishedState);

            playTone("finish");

            currentPomodoroState = FINISHED;
            finishDisplayEndTime = now + 10000;

            if (finishedState == RUNNING_WORK)
            {
                pomodoroWorkSessionCount++;
                webSocket.broadcastTXT("POMODORO_DONE:" + String(pomodoroWorkSessionCount));

                if (pomodoroWorkSessionCount % deviceSettings.workSessionsBeforeLongBreak == 0)
                {
                    Serial.println("Starting Long Break");
                    startPomodoro(RUNNING_LONG_BREAK, deviceSettings.longBreakDuration);
                }
                else
                {
                    Serial.println("Starting Short Break");
                    startPomodoro(RUNNING_SHORT_BREAK, deviceSettings.shortBreakDuration);
                }
            }
            else
            {
                webSocket.broadcastTXT("BREAK_DONE");
                Serial.println("Break finished, starting next Work session");
                startPomodoro(RUNNING_WORK, deviceSettings.workDuration);
            }
        }
        else
        {
        }
    }
    else if (currentPomodoroState == PAUSED_WORK || currentPomodoroState == PAUSED_SHORT_BREAK || currentPomodoroState == PAUSED_LONG_BREAK)
    {
    }
    else if (currentPomodoroState == IDLE)
    {
    }
}

void updateDisplay()
{
    String line1 = "";
    String line2 = "";
    unsigned long now = millis();

    bool inPausedState = (currentPomodoroState == PAUSED_WORK || currentPomodoroState == PAUSED_SHORT_BREAK || currentPomodoroState == PAUSED_LONG_BREAK);
    bool isRunningState = (currentPomodoroState == RUNNING_WORK || currentPomodoroState == RUNNING_SHORT_BREAK || currentPomodoroState == RUNNING_LONG_BREAK);

    if (now - lastBlinkToggleTime >= BLINK_INTERVAL)
    {
        blinkState = !blinkState;
        lastBlinkToggleTime = now;
    }

    char timeStr[6];
    bool validTime = false;
    if (getLocalTime(&timeinfo))
    {
        sprintf(timeStr, "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
        validTime = true;
    }
    else
    {
        strcpy(timeStr, "--:--");
    }

    if (currentUIMode == CLOCK_MODE)
    {

        if (now - lastClockUpdateTime >= CLOCK_UPDATE_INTERVAL)
        {
            lastClockUpdateTime = now;
            if (getLocalTime(&timeinfo))
            {

                sprintf(timeStr, "%02d:%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
                line1 = String(timeStr);

                char dateStr[16];
                sprintf(dateStr, "%02d-%02d-%04d", timeinfo.tm_mday, timeinfo.tm_mon + 1, timeinfo.tm_year + 1900);
                line2 = String(dateStr);
            }
            else
            {

                line1 = "Clock Error!";
                line2 = "Check WiFi";
            }
            updateLcdRaw(line1, line2);
        }
        return;
    }
    else if (currentUIMode == WIFI_MODE)
    {

        if (WiFi.status() == WL_CONNECTED)
        {
            line1 = "SSID: " + WiFi.SSID();
            line2 = WiFi.localIP().toString();
        }
        else
        {
            line1 = "Not Connected";
            line2 = "Press < for Setup";
        }
        updateLcdRaw(line1, line2);
        return;
    }

    int remainingSeconds = 0;
    if (currentPomodoroState != IDLE && currentPomodoroState != FINISHED)
    {
        if (currentPomodoroState == PAUSED_WORK || currentPomodoroState == PAUSED_SHORT_BREAK || currentPomodoroState == PAUSED_LONG_BREAK)
        {

            unsigned long elapsedBeforePause = pomodoroPauseStartTime - pomodoroStartTime;
            remainingSeconds = currentPomodoroDuration - (elapsedBeforePause / 1000);
        }
        else
        {

            if (now < pomodoroTargetTime)
            {
                remainingSeconds = (pomodoroTargetTime - now + 500) / 1000;
            }
            else
            {
                remainingSeconds = 0;
            }
        }
        if (remainingSeconds < 0)
            remainingSeconds = 0;
    }

    switch (currentPomodoroState)
    {
    case IDLE:
        line1 = "Ready";

        if (strlen(activeTaskId) > 0)
        {
            int idx = findTaskById(activeTaskId);
            if (idx != -1)
                line2 = tasks[idx].title;
            else
                line2 = "Select Task";
        }
        else
        {
            line2 = "Start Timer";
        }
        break;

    case RUNNING_WORK:
    case PAUSED_WORK:

        line1 = "\1 ";

        if (!inPausedState || (inPausedState && blinkState))
        {
            char timeBuffer[6];
            sprintf(timeBuffer, "%02d:%02d", remainingSeconds / 60, remainingSeconds % 60);
            line1 += timeBuffer;
        }
        else
        {
            line1 += "  :  ";
        }

        if (validTime)
        {

            while (line1.length() < 10)
            {
                line1 += " ";
            }
            line1 += timeStr;
        }

        if (deviceSettings.focusEnabled)
        {
            if (focusModeActivePause)
            {

                while (line1.length() < 15)
                    line1 += " ";
                line1 += "!";
            }
            else if (isRunningState)
            {

                while (line1.length() < 15)
                    line1 += " ";
                line1 += "\0";
            }
        }

        if (strlen(activeTaskId) > 0)
        {
            int idx = findTaskById(activeTaskId);
            if (idx != -1)
            {
                line2 = tasks[idx].title;
            }
            else
            {
                line2 = "Work";
            }
        }
        else
        {

            line2 = "Work #" + String(pomodoroWorkSessionCount + 1);
        }
        break;

    case RUNNING_SHORT_BREAK:
    case PAUSED_SHORT_BREAK:

        line1 = "\1 ";

        if (!inPausedState || (inPausedState && blinkState))
        {
            char timeBuffer[6];
            sprintf(timeBuffer, "%02d:%02d", remainingSeconds / 60, remainingSeconds % 60);
            line1 += timeBuffer;
        }
        else
        {
            line1 += "  :  ";
        }

        if (validTime)
        {

            while (line1.length() < 10)
            {
                line1 += " ";
            }
            line1 += timeStr;
        }

        line2 = "Short Break";
        break;

    case RUNNING_LONG_BREAK:
    case PAUSED_LONG_BREAK:

        line1 = "\1 ";

        if (!inPausedState || (inPausedState && blinkState))
        {
            char timeBuffer[6];
            sprintf(timeBuffer, "%02d:%02d", remainingSeconds / 60, remainingSeconds % 60);
            line1 += timeBuffer;
        }
        else
        {
            line1 += "  :  ";
        }

        if (validTime)
        {

            while (line1.length() < 10)
            {
                line1 += " ";
            }
            line1 += timeStr;
        }

        line2 = "Long Break";
        break;

    case FINISHED:

        line1 = "00:00";
        line2 = "Finished!";
        break;
    }

    updateLcdRaw(line1, line2);
}

void getPomodoroStateAsJson(String &jsonOutput)
{
    DynamicJsonDocument doc(384);

    doc["state"] = (int)currentPomodoroState;
    doc["workSessionCount"] = pomodoroWorkSessionCount;
    doc["focusPaused"] = focusModeActivePause;

    int remainingSeconds = 0;
    int duration = 0;
    if (currentPomodoroState != IDLE && currentPomodoroState != FINISHED)
    {
        duration = currentPomodoroDuration;
        if (currentPomodoroState == PAUSED_WORK || currentPomodoroState == PAUSED_SHORT_BREAK || currentPomodoroState == PAUSED_LONG_BREAK)
        {
            unsigned long elapsedBeforePause = pomodoroPauseStartTime - pomodoroStartTime;
            remainingSeconds = currentPomodoroDuration - (elapsedBeforePause / 1000);
        }
        else
        {
            unsigned long now = millis();
            if (now < pomodoroTargetTime)
            {
                remainingSeconds = (pomodoroTargetTime - now + 500) / 1000;
            }
            else
            {
                remainingSeconds = 0;
            }
        }
        if (remainingSeconds < 0)
            remainingSeconds = 0;
    }
    else
    {

        if ((pomodoroWorkSessionCount % deviceSettings.workSessionsBeforeLongBreak == 0) && pomodoroWorkSessionCount > 0)
        {
            duration = deviceSettings.longBreakDuration;
        }
        else if (pomodoroWorkSessionCount > 0)
        {
            duration = deviceSettings.workDuration;
        }
        else
        {

            duration = deviceSettings.workDuration;
        }
        remainingSeconds = duration;
    }

    doc["remaining"] = remainingSeconds;
    doc["duration"] = duration;

    if (strlen(activeTaskId) > 0)
    {
        int index = findTaskById(activeTaskId);
        if (index != -1)
        {
            JsonObject activeTask = doc.createNestedObject("activeTask");
            activeTask["id"] = tasks[index].id;
            activeTask["title"] = tasks[index].title;
        }
    }

    serializeJson(doc, jsonOutput);
}

void broadcastPomodoroState()
{
    String jsonState;
    getPomodoroStateAsJson(jsonState);
    webSocket.broadcastTXT("POMODORO_STATE:" + jsonState);
}

void sendFullState(uint8_t clientId)
{
    Serial.printf("Sending full state to client #%u\n", clientId);

    webSocket.sendTXT(clientId, "DEVICE:ESP32_POMO_V1.0");

    String settingsJson;
    getSettingsAsJson(settingsJson);
    webSocket.sendTXT(clientId, "SETTINGS:" + settingsJson);

    String tasksJson;
    getTasksAsJson(tasksJson);
    webSocket.sendTXT(clientId, "TASKS:" + tasksJson);

    String pomodoroJson;
    getPomodoroStateAsJson(pomodoroJson);
    webSocket.sendTXT(clientId, "POMODORO_STATE:" + pomodoroJson);

    if (!sensor.timeoutOccurred())
    {
        webSocket.sendTXT(clientId, "TOF_STATUS:OK");
    }
    else
    {
        webSocket.sendTXT(clientId, "TOF_STATUS:ERROR");
    }
}

void webSocketEvent(uint8_t num, WStype_t type, uint8_t *payload, size_t length)
{
    switch (type)
    {
    case WStype_DISCONNECTED:
        Serial.printf("[%u] DisConnected\n", num);
        break;
    case WStype_CONNECTED:
    {
        IPAddress ip = webSocket.remoteIP(num);
        Serial.printf("[%u] Connected from %d.%d.%d.%d url: %s\n", num, ip[0], ip[1], ip[2], ip[3], payload);

        sendFullState(num);
        break;
    }
    case WStype_TEXT:
    {
        Serial.printf("[%u] Received Text: %s\n", num, payload);

        char *message = new char[length + 1];
        memcpy(message, payload, length);
        message[length] = '\0';

        String messageStr = String(message);

        if (messageStr.startsWith("SETTINGS:"))
        {
            const char *settingsJson = message + 9;
            if (updateSettingsFromJson(settingsJson))
            {
                String updatedSettings;
                getSettingsAsJson(updatedSettings);
                webSocket.broadcastTXT("SETTINGS:" + updatedSettings);

                configTime(deviceSettings.timezone * 3600, 0, ntpServer);
                Serial.println("Applied new timezone configuration");
            }
        }
        else if (strcmp(message, "GET_SETTINGS") == 0)
        {
            String settingsJson;
            getSettingsAsJson(settingsJson);
            webSocket.sendTXT(num, "SETTINGS:" + settingsJson);
            Serial.println("Sent settings to client");
        }

        else if (messageStr.startsWith("TASK_ADD:"))
        {
            if (addTask(messageStr.substring(9).c_str()))
            {
                String tasksJson;
                getTasksAsJson(tasksJson);
                webSocket.broadcastTXT("TASKS:" + tasksJson);
            }
            else
                webSocket.sendTXT(num, "ERROR:Task list full");
        }
        else if (messageStr.startsWith("TASK_DELETE:"))
        {
            if (deleteTask(messageStr.substring(12).c_str()))
            {
                String tasksJson;
                getTasksAsJson(tasksJson);
                webSocket.broadcastTXT("TASKS:" + tasksJson);
                updateDisplay();
            }
            else
                webSocket.sendTXT(num, "ERROR:Task not found");
        }
        else if (messageStr.startsWith("TASK_TOGGLE:"))
        {
            if (toggleTaskCompletion(messageStr.substring(12).c_str()))
            {
                String tasksJson;
                getTasksAsJson(tasksJson);
                webSocket.broadcastTXT("TASKS:" + tasksJson);
                updateDisplay();
            }
            else
                webSocket.sendTXT(num, "ERROR:Task not found");
        }
        else if (messageStr.startsWith("TASK_ACTIVE:"))
        {
            if (setActiveTask(messageStr.substring(12).c_str()))
            {
                String tasksJson;
                getTasksAsJson(tasksJson);
                webSocket.broadcastTXT("TASKS:" + tasksJson);
                updateDisplay();
            }
            else
                webSocket.sendTXT(num, "ERROR:Task not found or completed");
        }
        else if (messageStr.startsWith("TASK_UPDATE:"))
        {
            String data = messageStr.substring(12);
            int colonPos = data.indexOf(':');
            if (colonPos > 0)
            {
                String id = data.substring(0, colonPos);
                String title = data.substring(colonPos + 1);
                if (updateTaskTitle(id.c_str(), title.c_str()))
                {
                    String tasksJson;
                    getTasksAsJson(tasksJson);
                    webSocket.broadcastTXT("TASKS:" + tasksJson);
                    updateDisplay();
                }
                else
                    webSocket.sendTXT(num, "ERROR:Task not found");
            }
        }

        else if (messageStr.startsWith("POMO:"))
        {
            handlePomodoroCommand(messageStr.substring(5));
        }

        else if (messageStr == "RESET_WIFI")
        {
            if (currentPomodoroState == IDLE)
            {

                webSocket.sendTXT(num, "WIFI_RESET:OK");

                delay(500);

                WiFiManager wifiManager;
                wifiManager.resetSettings();
                delay(1000);

                setupWifi(true);
            }
            else
            {

                webSocket.sendTXT(num, "WIFI_RESET:DENIED");
                webSocket.sendTXT(num, "ERROR:Cannot reset WiFi during active session");
            }
        }

        else if (messageStr == "RESTART")
        {
            webSocket.sendTXT(num, "RESTART:OK");

            delay(1000);

            ESP.restart();
        }

        else if (strcmp(message, "SYNC") == 0)
        {
            sendFullState(num);
        }
        else if (messageStr.startsWith("GET_TOF_STATUS"))
        {
            if (!sensor.timeoutOccurred())
            {
                webSocket.sendTXT(num, "TOF_STATUS:OK");
            }
            else
            {
                webSocket.sendTXT(num, "TOF_STATUS:ERROR");
            }
        }

        if (messageStr == "FOCUS_OVERRIDE")
        {
            if (focusModeActivePause)
            {
                focusModeActivePause = false;
                focusAlertSounded = false;
                if (currentPomodoroState == PAUSED_WORK)
                {
                    resumePomodoro();
                }
                webSocket.sendTXT(num, "FOCUS_OVERRIDE:OK");
            }
            else
            {
                webSocket.sendTXT(num, "FOCUS_OVERRIDE:NOT_NEEDED");
            }
        }

        delete[] message;
        break;
    }
    case WStype_BIN:
        Serial.printf("[%u] Received Binary, length: %u\n", num, length);
        break;
    case WStype_ERROR:
    case WStype_FRAGMENT_TEXT_START:
    case WStype_FRAGMENT_BIN_START:
    case WStype_FRAGMENT:
    case WStype_FRAGMENT_FIN:
    case WStype_PING:
    case WStype_PONG:
        Serial.printf("[%u] WS Event Type: %d\n", num, type);
        break;
    }
}

void handlePomodoroCommand(String command)
{
    Serial.print("Handling Pomodoro Command: ");
    Serial.println(command);
    if (command.startsWith("START_WORK"))
    {

        int duration = deviceSettings.workDuration;
        int colonIndex = command.indexOf(':');
        if (colonIndex > 0)
        {
            duration = command.substring(colonIndex + 1).toInt();
            if (duration <= 0)
                duration = deviceSettings.workDuration;
        }
        startPomodoro(RUNNING_WORK, duration);
    }
    else if (command.startsWith("START_SHORT_BREAK"))
    {
        int duration = deviceSettings.shortBreakDuration;
        int colonIndex = command.indexOf(':');
        if (colonIndex > 0)
        {
            duration = command.substring(colonIndex + 1).toInt();
            if (duration <= 0)
                duration = deviceSettings.shortBreakDuration;
        }
        startPomodoro(RUNNING_SHORT_BREAK, duration);
    }
    else if (command.startsWith("START_LONG_BREAK"))
    {
        int duration = deviceSettings.longBreakDuration;
        int colonIndex = command.indexOf(':');
        if (colonIndex > 0)
        {
            duration = command.substring(colonIndex + 1).toInt();
            if (duration <= 0)
                duration = deviceSettings.longBreakDuration;
        }
        startPomodoro(RUNNING_LONG_BREAK, duration);
    }
    else if (command == "PAUSE")
    {
        pausePomodoro();
    }
    else if (command == "RESUME")
    {
        resumePomodoro();
    }
    else if (command == "STOP")
    {
        stopPomodoro();
    }
}

void handleS1Button(bool isLongPress)
{
    playTone(isLongPress ? "longpress" : "button");
    if (currentUIMode == POMODORO_MODE)
    {
        if (currentPomodoroState != IDLE)
        {

            stopPomodoro();
        }

        currentUIMode = CLOCK_MODE;
    }
    else if (currentUIMode == WIFI_MODE)
    {

        currentUIMode = CLOCK_MODE;
    }
    else
    {

        currentUIMode = WIFI_MODE;
    }
    updateDisplay();
}

void handleS2Button(bool isLongPress)
{
    playTone(isLongPress ? "longpress" : "button");

    if (currentPomodoroState == IDLE)
    {

        switch (currentUIMode)
        {
        case CLOCK_MODE:
            currentUIMode = POMODORO_MODE;
            break;
        case POMODORO_MODE:
            currentUIMode = WIFI_MODE;
            break;
        case WIFI_MODE:
            currentUIMode = CLOCK_MODE;
            break;
        }
        updateDisplay();
        return;
    }

    if (currentUIMode == POMODORO_MODE)
    {
        if (isLongPress && (currentPomodoroState == RUNNING_WORK || currentPomodoroState == PAUSED_WORK))
        {

            int currentTaskIndex = -1;
            if (strlen(activeTaskId) > 0)
            {
                currentTaskIndex = findTaskById(activeTaskId);
            }
            int nextTaskIndex = -1;
            for (int i = 0; i < taskCount; i++)
            {
                int checkIndex = (currentTaskIndex + i + 1) % taskCount;
                if (!tasks[checkIndex].completed)
                {
                    nextTaskIndex = checkIndex;
                    break;
                }
            }
            if (nextTaskIndex != -1)
            {
                setActiveTask(tasks[nextTaskIndex].id);
                updateDisplay();
            }
        }
    }
}

void handleS3Button(bool isLongPress)
{
    playTone(isLongPress ? "longpress" : "button");
    if (currentUIMode == POMODORO_MODE)
    {
        if (currentPomodoroState == IDLE)
        {

            startPomodoro(RUNNING_WORK, deviceSettings.workDuration);
        }
        else if (currentPomodoroState == RUNNING_WORK || currentPomodoroState == RUNNING_SHORT_BREAK || currentPomodoroState == RUNNING_LONG_BREAK)
        {

            pausePomodoro();
        }
        else if (currentPomodoroState == PAUSED_WORK || currentPomodoroState == PAUSED_SHORT_BREAK || currentPomodoroState == PAUSED_LONG_BREAK)
        {

            resumePomodoro();
        }
    }
    else if (currentUIMode == CLOCK_MODE)
    {

        currentUIMode = POMODORO_MODE;
        updateDisplay();
    }
    else if (currentUIMode == WIFI_MODE)
    {

        if (WiFi.status() != WL_CONNECTED)
        {

            updateLcdRaw("Starting WiFi", "Setup Portal...");
            delay(1000);
            setupWifi(true);
        }
    }
}

void handleS4Button(bool isLongPress)
{
    playTone(isLongPress ? "longpress" : "button");

    if (currentPomodoroState == IDLE)
    {

        switch (currentUIMode)
        {
        case CLOCK_MODE:
            currentUIMode = POMODORO_MODE;
            break;
        case POMODORO_MODE:
            currentUIMode = WIFI_MODE;
            break;
        case WIFI_MODE:
            currentUIMode = CLOCK_MODE;
            break;
        }
        updateDisplay();
        return;
    }

    if (currentUIMode == POMODORO_MODE)
    {
        if (isLongPress && (currentPomodoroState == RUNNING_WORK || currentPomodoroState == PAUSED_WORK))
        {

            if (strlen(activeTaskId) > 0)
            {
                toggleTaskCompletion(activeTaskId);

                int nextTaskIndex = -1;
                for (int i = 0; i < taskCount; i++)
                {
                    if (!tasks[i].completed)
                    {
                        nextTaskIndex = i;
                        break;
                    }
                }

                if (nextTaskIndex != -1)
                {
                    setActiveTask(tasks[nextTaskIndex].id);
                }
                else
                {
                    setActiveTask("");
                }
                updateDisplay();
            }
        }
    }
}

void checkPhysicalButton()
{

    static unsigned long lastButtonCheckTime = 0;
    static int lastS1State = HIGH;
    static int lastS2State = HIGH;
    static int lastS3State = HIGH;
    static int lastS4State = HIGH;

    static bool s1Held = false;
    static bool s2Held = false;
    static bool s3Held = false;
    static bool s4Held = false;

    static unsigned long s1PressTime = 0;
    static unsigned long s2PressTime = 0;
    static unsigned long s3PressTime = 0;
    static unsigned long s4PressTime = 0;

    static bool s1LongTriggered = false;
    static bool s2LongTriggered = false;
    static bool s3LongTriggered = false;
    static bool s4LongTriggered = false;

    const unsigned long longPressTime = 800;
    const unsigned long debounceTime = 20;

    if (millis() - lastButtonCheckTime < debounceTime)
        return;
    lastButtonCheckTime = millis();

    int currentS1State = digitalRead(S1_PIN);
    int currentS2State = digitalRead(S2_PIN);
    int currentS3State = digitalRead(S3_PIN);
    int currentS4State = digitalRead(S4_PIN);

    if (currentS1State == LOW && lastS1State == HIGH)
    {

        s1Held = true;
        s1PressTime = millis();
        s1LongTriggered = false;
        Serial.println("S1 (Back) Button Down");
    }
    else if (currentS1State == LOW && s1Held)
    {

        if (!s1LongTriggered && (millis() - s1PressTime >= longPressTime))
        {
            handleS1Button(true);
            s1LongTriggered = true;
        }
    }
    else if (currentS1State == HIGH && lastS1State == LOW)
    {

        Serial.println("S1 (Back) Button Up");
        if (s1Held && !s1LongTriggered)
        {
            handleS1Button(false);
        }
        s1Held = false;
    }

    if (currentS2State == LOW && lastS2State == HIGH)
    {
        s2Held = true;
        s2PressTime = millis();
        s2LongTriggered = false;
        Serial.println("S2 (Right) Button Down");
    }
    else if (currentS2State == LOW && s2Held)
    {
        if (!s2LongTriggered && (millis() - s2PressTime >= longPressTime))
        {
            handleS2Button(true);
            s2LongTriggered = true;
        }
    }
    else if (currentS2State == HIGH && lastS2State == LOW)
    {
        Serial.println("S2 (Right) Button Up");
        if (s2Held && !s2LongTriggered)
        {
            handleS2Button(false);
        }
        s2Held = false;
    }

    if (currentS3State == LOW && lastS3State == HIGH)
    {
        s3Held = true;
        s3PressTime = millis();
        s3LongTriggered = false;
        Serial.println("S3 (Center) Button Down");
    }
    else if (currentS3State == LOW && s3Held)
    {
        if (!s3LongTriggered && (millis() - s3PressTime >= longPressTime))
        {
            handleS3Button(true);
            s3LongTriggered = true;
        }
    }
    else if (currentS3State == HIGH && lastS3State == LOW)
    {
        Serial.println("S3 (Center) Button Up");
        if (s3Held && !s3LongTriggered)
        {
            handleS3Button(false);
        }
        s3Held = false;
    }

    if (currentS4State == LOW && lastS4State == HIGH)
    {
        s4Held = true;
        s4PressTime = millis();
        s4LongTriggered = false;
        Serial.println("S4 (Left) Button Down");
    }
    else if (currentS4State == LOW && s4Held)
    {
        if (!s4LongTriggered && (millis() - s4PressTime >= longPressTime))
        {
            handleS4Button(true);
            s4LongTriggered = true;
        }
    }
    else if (currentS4State == HIGH && lastS4State == LOW)
    {
        Serial.println("S4 (Left) Button Up");
        if (s4Held && !s4LongTriggered)
        {
            handleS4Button(false);
        }
        s4Held = false;
    }

    static bool s1s4Combo = false;
    static unsigned long s1s4ComboStartTime = 0;
    const unsigned long wifiResetLongPress = 3000;

    if (currentS1State == LOW && currentS4State == LOW)
    {
        if (!s1s4Combo)
        {
            s1s4Combo = true;
            s1s4ComboStartTime = millis();
            Serial.println("S1+S4 combo detected, waiting for long press");
        }
        else if (millis() - s1s4ComboStartTime >= wifiResetLongPress)
        {

            handleWiFiReset();
            s1s4Combo = false;
        }
    }
    else
    {
        s1s4Combo = false;
    }

    lastS1State = currentS1State;
    lastS2State = currentS2State;
    lastS3State = currentS3State;
    lastS4State = currentS4State;
}

void updateVL53L0X()
{
    if (millis() - lastDistanceReadTime >= DISTANCE_READ_INTERVAL)
    {
        lastDistanceReadTime = millis();
        int currentDistance = sensor.readRangeContinuousMillimeters();
        if (!sensor.timeoutOccurred())
        {
            if (currentDistance != lastReportedDistance)
            {
                Serial.print("Distance changed: ");
                Serial.print(currentDistance);
                Serial.println(" mm");
                if (webSocket.connectedClients() > 0)
                {
                    char distBuffer[20];
                    sprintf(distBuffer, "DIST:%d", currentDistance);
                    webSocket.broadcastTXT(distBuffer);
                }
                lastReportedDistance = currentDistance;
            }
        }
        else
        {

            if (lastReportedDistance != -999)
            {
                Serial.println("VL53L0X Timeout");
                if (webSocket.connectedClients() > 0)
                {
                    webSocket.broadcastTXT("DIST:TIMEOUT");
                }
                lastReportedDistance = -999;
            }
        }
    }
}

void checkFocusMode()
{

    if (!deviceSettings.focusEnabled)
    {

        if (focusModeActivePause)
        {
            focusModeActivePause = false;

            resumePomodoro();
        }
        return;
    }

    bool isWorkSession = (currentPomodoroState == RUNNING_WORK || currentPomodoroState == PAUSED_WORK);
    if (!isWorkSession)
    {
        return;
    }

    int currentDistance = sensor.readRangeContinuousMillimeters();
    if (sensor.timeoutOccurred())
    {

        return;
    }

    if (currentDistance < deviceSettings.distanceCalibration)
    {

        if (focusModeActivePause)
        {
            focusModeActivePause = false;
            focusAlertSounded = false;
            resumePomodoro();
            Serial.println("Focus mode: User back at desk, resuming timer");
        }
    }
    else
    {

        if (currentPomodoroState == RUNNING_WORK && !focusModeActivePause)
        {
            focusModeActivePause = true;
            pausePomodoro();
            Serial.println("Focus mode: User left desk, pausing timer");

            if (!focusAlertSounded)
            {
                focusAlertSounded = true;
                playFocusAlert();
            }
        }
    }
}

void playFocusAlert()
{
#ifdef BUZZER_PIN
    pinMode(BUZZER_PIN, OUTPUT);

    for (int i = 0; i < 3; i++)
    {
        digitalWrite(BUZZER_PIN, HIGH);
        delay(150);
        digitalWrite(BUZZER_PIN, LOW);

        if (i < 2)
        {
            delay(150);
        }
    }
#else
    Serial.println("FOCUS_ALERT: 3 beeps");
#endif
}

void setup()
{
    Serial.begin(115200);
    Serial.println("\n--- ESP32 Pomodoro Clock Booting ---");

    showBootStatus("Starting...");

    showBootStatus("Loading settings");
    loadSettings();

    showBootStatus("Init hardware");
    Wire.begin(SDA_PIN, SCL_PIN);
    scanI2C();

    lcd.begin(16, 2);
    lcd.backlight();

    initCustomCharacters(lcd);

    showBootStatus("Init sensor");
    Serial.println("Initializing VL53L0X...");
    pinMode(XSHUT_PIN, OUTPUT);
    digitalWrite(XSHUT_PIN, LOW);
    delay(10);
    digitalWrite(XSHUT_PIN, HIGH);
    delay(10);
    if (!sensor.init())
    {
        Serial.println("Failed to initialize VL53L0X!");
        showBootStatus("Sensor Error!");
        delay(2000);
    }
    else
    {
        Serial.println("VL53L0X Initialized.");

        sensor.setTimeout(200);

        sensor.setMeasurementTimingBudget(20000);
        sensor.startContinuous(50);
    }

#ifdef BTN_PIN
    pinMode(BTN_PIN, INPUT_PULLUP);
    Serial.println("Button Initialized (Pin " + String(BTN_PIN) + ")");
#endif

    showBootStatus("Init buttons");
    pinMode(S1_PIN, INPUT_PULLUP);
    pinMode(S2_PIN, INPUT_PULLUP);
    pinMode(S3_PIN, INPUT_PULLUP);
    pinMode(S4_PIN, INPUT_PULLUP);
    Serial.println("Physical Buttons Initialized (S1-S4)");

#ifdef BUZZER_PIN
    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW);
    Serial.println("Buzzer Initialized (Pin " + String(BUZZER_PIN) + ")");

    playTone("startup");
#endif

    showBootStatus("Connecting WiFi");
    if (!setupWifi(false))
    {
        Serial.println("Failed to connect to WiFi with stored credentials, starting config portal");
        showBootStatus("WiFi Setup");
        setupWifi(true);
    }

    showBootStatus("Sync time");
    configTime(deviceSettings.timezone * 3600, 0, ntpServer);
    if (!getLocalTime(&timeinfo))
    {
        Serial.println("Failed to obtain time");
        showBootStatus("NTP failed!");
        delay(2000);
    }
    else
    {
        Serial.println("NTP initialized for Clock mode");
        Serial.println(&timeinfo, "Current time: %A, %B %d %Y %H:%M:%S");

        showBootStatus("Time synced");
        char timeStr[16];
        sprintf(timeStr, "%02d:%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
        char dateStr[16];
        sprintf(dateStr, "%02d-%02d-%04d", timeinfo.tm_mday, timeinfo.tm_mon + 1, timeinfo.tm_year + 1900);
        updateLcdRaw(timeStr, dateStr);
        cachedLcdLine1 = timeStr;
        cachedLcdLine2 = dateStr;
        delay(1500);
    }

    showBootStatus("Starting server");
    webSocket.begin();
    webSocket.onEvent(webSocketEvent);
    Serial.println("WebSocket Server Started");

    showBootStatus("Init tasks");
    randomSeed(analogRead(A0));

    showBootStatus("Ready!");
    delay(1000);

    currentPomodoroState = IDLE;
    cachedLcdLine1 = "";
    cachedLcdLine2 = "";
    updateDisplay();
    Serial.println("Setup Complete. Entering main loop.");
}

void saveLCDBackup()
{
    if (!lcdBackupSaved)
    {
        backupLcdLine1 = cachedLcdLine1;
        backupLcdLine2 = cachedLcdLine2;
        lcdBackupSaved = true;
    }
}

void restoreLCDBackup()
{
    if (lcdBackupSaved)
    {
        updateLcdRaw(backupLcdLine1, backupLcdLine2);
        cachedLcdLine1 = backupLcdLine1;
        cachedLcdLine2 = backupLcdLine2;
        lcdBackupSaved = false;
    }
}

bool setupWifi(bool forceConfig)
{
    WiFiManager wifiManager;

    wifiManager.setAPCallback([](WiFiManager *myWiFiManager)
                              {
    Serial.println("Entered config mode");
    Serial.println(WiFi.softAPIP());
    saveLCDBackup();
    updateLcdRaw("PomoMentor", "WiFi Setup Mode");
    Serial.print("Connect to ");
    Serial.println(myWiFiManager->getConfigPortalSSID());
    lcd.setCursor(0, 0);
    lcd.print("Setup Mode ");
    lcd.setCursor(0, 1);
    lcd.print(myWiFiManager->getConfigPortalSSID());
    portalActive = true;
    playTone("longpress"); 
    playTone("longpress"); });

    wifiManager.setSaveConfigCallback([]()
                                      {
    Serial.println("WiFi credentials saved");
    updateLcdRaw("PomoMentor", "WiFi Saved!");
    lcd.setCursor(0, 0);
    lcd.print("PomoMentor      ");
    lcd.setCursor(0, 1);
    lcd.print("WiFi Saved!     ");
    playTone("finish"); });

    wifiManager.setConfigPortalTimeoutCallback([]()
                                               {
    Serial.println("WiFi config portal timeout");
    updateLcdRaw("PomoMentor", "WiFi Timeout!");
    lcd.setCursor(0, 0);
    lcd.print("PomoMentor      ");
    lcd.setCursor(0, 1);
    lcd.print("WiFi Timeout!   ");
    playTone("stop"); 
    portalActive = false;
    delay(2000); });

    wifiManager.setConfigPortalTimeout(WIFI_CONFIG_TIMEOUT);
    wifiManager.setConnectTimeout(WIFI_CONNECT_TIMEOUT);

    bool connected = false;
    if (forceConfig)
    {
        connected = wifiManager.startConfigPortal(AP_NAME, AP_PASSWORD);
    }
    else
    {
        connected = wifiManager.autoConnect(AP_NAME, AP_PASSWORD);
    }

    portalActive = false;

    if (connected)
    {
        Serial.println("WiFi Connected");
        Serial.print("IP Address: ");
        Serial.println(WiFi.localIP());
        updateLcdRaw("PomoMentor", "WiFi Connected");
        cachedLcdLine1 = "PomoMentor";
        cachedLcdLine2 = "WiFi Connected";
        delay(1500);

        updateLcdRaw("PomoMentor", WiFi.localIP().toString());
        cachedLcdLine1 = "PomoMentor";
        cachedLcdLine2 = WiFi.localIP().toString();
        delay(1500);

        restoreLCDBackup();
        return true;
    }
    else
    {
        Serial.println("Failed to connect to WiFi");
        updateLcdRaw("PomoMentor", "WiFi Failed!");
        cachedLcdLine1 = "PomoMentor";
        cachedLcdLine2 = "WiFi Failed!";
        delay(2000);

        restoreLCDBackup();
        return false;
    }
}

void handleWiFiReset()
{

    if (currentPomodoroState == IDLE)
    {
        Serial.println("WiFi settings reset requested");
        updateLcdRaw("PomoMentor", "WiFi Reset...");
        cachedLcdLine1 = "PomoMentor";
        cachedLcdLine2 = "WiFi Reset...";

        playTone("longpress");
        delay(500);
        playTone("longpress");

        WiFiManager wifiManager;
        wifiManager.resetSettings();
        delay(1000);

        updateLcdRaw("PomoMentor", "Connect to setup");
        delay(1000);

        setupWifi(true);
    }
    else
    {
        Serial.println("WiFi reset denied: Pomodoro active");
        updateLcdRaw("PomoMentor", "Stop timer first");
        cachedLcdLine1 = "PomoMentor";
        cachedLcdLine2 = "Stop timer first";
        delay(2000);

        cachedLcdLine1 = "";
        cachedLcdLine2 = "";
        updateDisplay();
    }
}

void loop()
{

    if (portalActive)
    {
        digitalWrite(BUZZER_PIN, LOW);

        delay(50);
        return;
    }

    static unsigned long lastWifiCheckTime = 0;
    static int wifiReconnectAttempts = 0;
    static bool wasConnected = false;

    if (millis() - lastWifiCheckTime > 15000)
    {
        lastWifiCheckTime = millis();

        if (WiFi.status() != WL_CONNECTED)
        {

            if (wasConnected)
            {
                Serial.println("WiFi connection lost");
                updateLcdRaw("WiFi Disconnected", "Reconnecting...");
                delay(1000);
                wasConnected = false;
            }

            if (wifiReconnectAttempts < 5)
            {
                Serial.printf("Reconnecting to WiFi (attempt %d/5)...\n", wifiReconnectAttempts + 1);
                WiFi.reconnect();
                wifiReconnectAttempts++;

                for (int i = 0; i < 5; i++)
                {
                    if (WiFi.status() == WL_CONNECTED)
                    {
                        Serial.println("WiFi reConnected");
                        updateLcdRaw("WiFi Reconnected", WiFi.localIP().toString());
                        delay(1000);
                        wasConnected = true;
                        wifiReconnectAttempts = 0;
                        break;
                    }
                    delay(200);
                }
            }
            else if (wifiReconnectAttempts == 5)
            {

                Serial.println("WiFi reconnection failed after 5 attempts");
                updateLcdRaw("WiFi Failed", "Operating offline");
                delay(2000);
                wifiReconnectAttempts++;
            }

            restoreLCDBackup();
        }
        else
        {
            wasConnected = true;
            wifiReconnectAttempts = 0;
        }
    }

    checkPhysicalButton();
    updateVL53L0X();
    webSocket.loop();
    updatePomodoro();
    updateDisplay();
    checkFocusMode();

    delay(10);
}