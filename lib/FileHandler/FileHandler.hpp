#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <FS.h>

class FileHandler {
public:
    FileHandler(fs::FS& fs);

    bool createFile(const char* filePath);
    bool readJson(const char* filePath, JsonDocument& doc);
    bool writeJson(const char* filePath, const JsonDocument& doc);
private:
    fs::FS& m_fs;
};