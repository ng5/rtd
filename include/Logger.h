#pragma once
#include <chrono>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <shlobj.h>
#include <sstream>
#include <string>
#include <windows.h>

class Logger {
    std::string m_logFilePath;
    std::ofstream m_logFile;
    std::mutex m_mutex;
    bool m_enabled;

    static std::string GetUserHomeDirectory() {
        char path[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_PROFILE, nullptr, 0, path))) {
            return {path};
        }
        return {};
    }

    static std::string GetTimestamp() {
        using namespace std::chrono;
        auto now = system_clock::now();
        auto tt = system_clock::to_time_t(now);
        auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;

        std::tm tm{};
        localtime_s(&tm, &tt);

        std::ostringstream ss;
        ss << std::setfill('0') << std::setw(4) << (tm.tm_year + 1900) << "-" << std::setw(2) << (tm.tm_mon + 1) << "-"
           << std::setw(2) << tm.tm_mday << " " << std::setw(2) << tm.tm_hour << ":" << std::setw(2) << tm.tm_min << ":"
           << std::setw(2) << tm.tm_sec << "." << std::setw(3) << ms.count();
        return ss.str();
    }

    static std::string GetFileTimestamp() {
        auto now = std::chrono::system_clock::now();
        auto tt = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
        localtime_s(&tm, &tt);
        std::ostringstream ss;
        ss << std::setfill('0') << std::setw(4) << (tm.tm_year + 1900) << std::setw(2) << (tm.tm_mon + 1)
           << std::setw(2) << tm.tm_mday << "_" << std::setw(2) << tm.tm_hour << std::setw(2) << tm.tm_min
           << std::setw(2) << tm.tm_sec;
        return ss.str();
    }

    [[nodiscard]] bool EnsureLogDirectory() const {
        auto homeDir = GetUserHomeDirectory();
        if (homeDir.empty())
            return false;

        auto logDir = homeDir + "\\RTDLogs";

        auto attrs = GetFileAttributesA(logDir.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES) {
            if (!CreateDirectoryA(logDir.c_str(), nullptr)) {
                return false;
            }
        }

        return true;
    }

  public:
    Logger() : m_enabled(false) {
        if (!EnsureLogDirectory())
            return;

        auto homeDir = GetUserHomeDirectory();
        if (homeDir.empty())
            return;

        auto logDir = homeDir + "\\RTDLogs";
        auto fileName = std::string("RTD_") + GetFileTimestamp() + ".log";
        m_logFilePath = logDir + "\\" + fileName;

        m_logFile.open(m_logFilePath, std::ios::out | std::ios::app);
        if (m_logFile.is_open()) {
            m_enabled = true;
            WriteHeader();
        }
    }

    Logger(const Logger &other) = delete;
    Logger(Logger &&other) noexcept = delete;
    Logger &operator=(const Logger &other) = delete;
    Logger &operator=(Logger &&other) noexcept = delete;

    ~Logger() {
        if (m_logFile.is_open()) {
            m_logFile.close();
        }
    }

    void WriteHeader() {
        if (!m_enabled)
            return;
        std::lock_guard lock(m_mutex);

        m_logFile << "========================================\n";
        m_logFile << "RTD Server Log - Session Started\n";
        m_logFile << "Timestamp: " << GetTimestamp() << "\n";
        m_logFile << "========================================\n\n";
        m_logFile.flush();
    }

    void LogInfo(const std::string &message) {
        if (!m_enabled)
            return;
        std::lock_guard lock(m_mutex);
        auto timestamp = GetTimestamp();
        auto logLine = "[" + timestamp + "] INFO: " + message + "\n";
        m_logFile << logLine;
        m_logFile.flush();
    }

    void LogSubscription(long topicId, const std::string &url, const std::string &topic) {
        if (!m_enabled)
            return;

        auto message = std::string{};
        if (url.starts_with("ws://") || url.starts_with("wss://")) {
            message = "SUBSCRIBE: TopicID=" + std::to_string(topicId) + ", URL='" + url + "', Topic='" + topic + "'";
        } else {
            message = "SUBSCRIBE: TopicID=" + std::to_string(topicId) + ", Mode=LEGACY, Param='" + url + "'";
        }
        LogInfo(message);
    }

    void LogUnsubscribe(long topicId) {
        if (!m_enabled)
            return;
        LogInfo(std::string("UNSUBSCRIBE: TopicID=") + std::to_string(topicId));
    }

    void LogDataReceived(long topicId, double value, const std::string &source) {
        if (!m_enabled)
            return;
        auto ss = std::ostringstream{};
        ss << "DATA_RECEIVED: TopicID=" << topicId << ", Value=" << std::fixed << std::setprecision(4) << value
           << ", Source='" << source << "'";
        LogInfo(ss.str());
    }

    void LogWebSocketConnect(const std::string &url) {
        if (!m_enabled)
            return;
        LogInfo(std::string("WEBSOCKET_CONNECT: URL='") + url + "'");
    }

    void LogWebSocketDisconnect(const std::string &url) {
        if (!m_enabled)
            return;
        LogInfo(std::string("WEBSOCKET_DISCONNECT: URL='") + url + "'");
    }

    void LogWebSocketMessage(const std::string &url, const std::string &message) {
        if (!m_enabled)
            return;
        LogInfo(std::string("WEBSOCKET_MESSAGE: URL='") + url + "', Data='" + message + "'");
    }

    void LogServerStart() {
        if (!m_enabled)
            return;
        LogInfo("SERVER_START: RTD Server initialized");
    }

    void LogServerTerminate() {
        if (!m_enabled)
            return;
        LogInfo("SERVER_TERMINATE: RTD Server shutting down");
    }
    void LogError(const std::string &error) {
        if (!m_enabled)
            return;
        std::lock_guard lock(m_mutex);
        auto timestamp = GetTimestamp();
        auto logLine = "[" + timestamp + "] ERROR: " + error + "\n";
        m_logFile << logLine;
        m_logFile.flush();
    }

    void LogError(const char *error) { LogError(std::string(error ? error : "")); }

  private:
};

inline Logger &GetLogger() {
    static Logger logger;
    return logger;
}
