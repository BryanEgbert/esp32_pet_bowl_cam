#include "FileHandler.hpp"

FileHandler::FileHandler(fs::FS& fs) 
    : m_fs(fs) {}

bool FileHandler::createFile(const char* filePath) {
    if (!this->m_fs.exists(filePath)) {
        File file = this->m_fs.open(filePath, FILE_WRITE);
        if (!file) {
            file.close();
            return false;
        }

        file.close();
    }

    return true;
}

bool FileHandler::readJson(const char* filePath, JsonDocument& doc) {
    if (!this->m_fs.exists(filePath)) {
        return false;
    }

    String fileContent;

    File file = this->m_fs.open(filePath);
    if (!file) {
      file.close();

      return false;
    }

    while (file.available()) {
      fileContent += char(file.read());
    }

    file.close();

    deserializeJson(doc, fileContent);

    return true;
}

bool FileHandler::writeJson(const char* filePath, const JsonDocument& doc) {
    File file = this->m_fs.open(filePath, FILE_WRITE);
    if (!file) {
      file.close();

      return false;
    }

    serializeJson(doc, file);
    
    file.close();

    return true;
}

bool FileHandler::writeJson(const char* filePath, const JsonArray& doc) {
    File file = this->m_fs.open(filePath, FILE_WRITE);
    if (!file) {
      file.close();

      return false;
    }

    serializeJson(doc, file);
    
    file.close();

    return true;
}

bool FileHandler::writeJson(const char* filePath, const JsonObject& doc) {
    File file = this->m_fs.open(filePath, FILE_WRITE);
    if (!file) {
      file.close();

      return false;
    }

    serializeJson(doc, file);
    
    file.close();

    return true;
}