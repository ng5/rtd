#pragma once
#include <chrono>
#include <ctime>
#include <format>
#include <fstream>
#include <mutex>
#include <shlobj.h>
#include <string>
#include <windows.h>

class Logger {
  private:
    std::wstring m_logFilePath;
    std::ofstream m_logFile;
    std::mutex m_mutex;
    bool m_enabled;
    static std::wstring ConvertToWString(const char *str) {
        if (!str) {
            return std::wstring();
        }
        int len = MultiByteToWideChar(CP_ACP, 0, str, -1, nullptr, 0); // Get the required length
        if (len == 0) {
            throw std::runtime_error("Conversion error");
        }
        std::wstring wstr(len, L'\0');
        MultiByteToWideChar(CP_ACP, 0, str, -1, &wstr[0], len);
        wstr.resize(len - 1); // Remove the null terminator
        return wstr;
    }
    static std::wstring GetUserHomeDirectory() {
        wchar_t path[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_PROFILE, NULL, 0, path))) {
            return path;
        }
        return L"";
    }

    static std::wstring GetTimestamp() {
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

        std::tm tm;
        localtime_s(&tm, &time_t);

        return std::format(L"{:04d}-{:02d}-{:02d} {:02d}:{:02d}:{:02d}.{:03d}", tm.tm_year + 1900, tm.tm_mon + 1,
                           tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, ms.count());
    }

    static std::wstring GetFileTimestamp() {
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);

        std::tm tm;
        localtime_s(&tm, &time_t);

        return std::format(L"{:04d}{:02d}{:02d}_{:02d}{:02d}{:02d}", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                           tm.tm_hour, tm.tm_min, tm.tm_sec);
    }

    [[nodiscard]] bool EnsureLogDirectory() const {
        std::wstring homeDir = GetUserHomeDirectory();
        if (homeDir.empty())
            return false;

        std::wstring logDir = homeDir + L"\\RTDLogs";

        // Create directory if it doesn't exist
        if (DWORD attrs = GetFileAttributesW(logDir.c_str()); attrs == INVALID_FILE_ATTRIBUTES) {
            if (!CreateDirectoryW(logDir.c_str(), nullptr)) {
                return false;
            }
        }

        return true;
    }

  public:
    Logger() : m_enabled(false) {
        if (!EnsureLogDirectory())
            return;

        std::wstring homeDir = GetUserHomeDirectory();
        if (homeDir.empty())
            return;

        std::wstring logDir = homeDir + L"\\RTDLogs";
        std::wstring fileName = std::format(L"RTD_{}.log", GetFileTimestamp());
        m_logFilePath = logDir + L"\\" + fileName;

        // Open log file
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
        m_logFile << "Timestamp: " << WideToUtf8(GetTimestamp()) << "\n";
        m_logFile << "========================================\n\n";
        m_logFile.flush();
    }

    void LogInfo(const std::wstring &message) {
        if (!m_enabled)
            return;
        std::lock_guard lock(m_mutex);
        std::wstring timestamp = GetTimestamp();
        std::wstring logLine = std::format(L"[{}] INFO: {}\n", timestamp, message);
        m_logFile << WideToUtf8(logLine);
        m_logFile.flush();
    }

    void LogSubscription(long topicId, const std::wstring &url, const std::wstring &topic) {
        if (!m_enabled)
            return;

        std::wstring message;
        if (url.starts_with(L"ws://") || url.starts_with(L"wss://")) {
            message = std::format(L"SUBSCRIBE: TopicID={}, URL='{}', Topic='{}'", topicId, url, topic);
        } else {
            message = std::format(L"SUBSCRIBE: TopicID={}, Mode=LEGACY, Param='{}'", topicId, url);
        }
        LogInfo(message);
    }

    void LogUnsubscribe(long topicId) {
        if (!m_enabled)
            return;
        LogInfo(std::format(L"UNSUBSCRIBE: TopicID={}", topicId));
    }

    void LogDataReceived(long topicId, double value, const std::wstring &source) {
        if (!m_enabled)
            return;
        LogInfo(std::format(L"DATA_RECEIVED: TopicID={}, Value={:.4f}, Source='{}'", topicId, value, source));
    }

    void LogWebSocketConnect(const std::wstring &url) {
        if (!m_enabled)
            return;
        LogInfo(std::format(L"WEBSOCKET_CONNECT: URL='{}'", url));
    }

    void LogWebSocketDisconnect(const std::wstring &url) {
        if (!m_enabled)
            return;
        LogInfo(std::format(L"WEBSOCKET_DISCONNECT: URL='{}'", url));
    }

    void LogWebSocketMessage(const std::wstring &url, const std::string &message) {
        if (!m_enabled)
            return;

        // Convert message to wstring for logging
        std::wstring wMessage(message.begin(), message.end());
        LogInfo(std::format(L"WEBSOCKET_MESSAGE: URL='{}', Data='{}'", url, wMessage));
    }

    void LogServerStart() {
        if (!m_enabled)
            return;
        LogInfo(L"SERVER_START: RTD Server initialized");
    }

    void LogServerTerminate() {
        if (!m_enabled)
            return;
        LogInfo(L"SERVER_TERMINATE: RTD Server shutting down");
    }
    void LogError(const std::wstring &error) {
        if (!m_enabled)
            return;
        std::lock_guard lock(m_mutex);
        std::wstring timestamp = GetTimestamp();
        std::wstring logLine = std::format(L"[{}] ERROR: {}\n", timestamp, error);
        m_logFile << WideToUtf8(logLine);
        m_logFile.flush();
    }
    void LogError(const std::string &error) { LogError(ConvertToWString(error.c_str())); }

  private:
    static std::string WideToUtf8(const std::wstring &wstr) {
        if (wstr.empty())
            return "";

        int size = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string result(size - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, result.data(), size, nullptr, nullptr);
        return result;
    }
};

// Global logger instance
inline Logger &GetLogger() {
    static Logger logger;
    return logger;
}
