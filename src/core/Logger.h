// =============================================================================
//  Logger.h — registro thread-safe: file su disco + buffer circolare per la UI
// =============================================================================
#pragma once

#include <deque>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace p2p {

enum class LogLevel : int { Debug = 0, Info, Warn, Error };

struct LogEntry {
    std::string time;
    LogLevel    level;
    std::string text;
};

class Logger {
public:
    static Logger& get();

    void openFile(const std::string& path);
    // rotazione: oltre maxBytes il file diventa path.1, path.1 -> path.2 ... (maxFiles in tutto)
    void setRotation(std::size_t maxBytes, int maxFiles);
    void write(LogLevel lvl, const std::string& text);
    std::vector<LogEntry> snapshot() const;
    std::size_t generation() const;   // cambia a ogni nuova riga
    void clear();
    const std::string& filePath() const { return path_; }

private:
    Logger() = default;
    mutable std::mutex   mtx_;
    std::deque<LogEntry> buf_;
    std::ofstream        file_;
    std::string          path_;
    std::size_t          gen_ = 0;
    std::size_t          fileBytes_ = 0;
    std::size_t          maxBytes_  = 5 * 1024 * 1024;
    int                  maxFiles_  = 5;
    void rotateLocked();
    static constexpr std::size_t kMax = 5000;
};

// concatena argomenti eterogenei: cat("Studio ", uid, " ok")
template <class... A>
std::string cat(A&&... a) {
    std::ostringstream os;
    (os << ... << a);
    return os.str();
}

template <class... A> void logDebug(A&&... a) { Logger::get().write(LogLevel::Debug, cat(a...)); }
template <class... A> void logInfo (A&&... a) { Logger::get().write(LogLevel::Info,  cat(a...)); }
template <class... A> void logWarn (A&&... a) { Logger::get().write(LogLevel::Warn,  cat(a...)); }
template <class... A> void logError(A&&... a) { Logger::get().write(LogLevel::Error, cat(a...)); }

} // namespace p2p
