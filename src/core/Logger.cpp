// =============================================================================
//  Logger.cpp
// =============================================================================
#include "Logger.h"
#include "DateUtil.h"

#include <filesystem>

namespace p2p {

namespace fs = std::filesystem;

Logger& Logger::get() {
    static Logger instance;
    return instance;
}

void Logger::openFile(const std::string& path) {
    std::lock_guard<std::mutex> lk(mtx_);
    path_ = path;
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    fileBytes_ = ec ? 0 : static_cast<std::size_t>(size);
    file_.open(path, std::ios::app);
    if (fileBytes_ >= maxBytes_) rotateLocked();
}

void Logger::setRotation(std::size_t maxBytes, int maxFiles) {
    std::lock_guard<std::mutex> lk(mtx_);
    maxBytes_ = maxBytes;
    maxFiles_ = maxFiles < 1 ? 1 : maxFiles;
}

// log -> log.1 -> log.2 ... ; il più vecchio viene eliminato
void Logger::rotateLocked() {
    if (path_.empty()) return;
    file_.close();
    std::error_code ec;
    fs::remove(path_ + "." + std::to_string(maxFiles_ - 1), ec);
    for (int i = maxFiles_ - 2; i >= 1; --i)
        fs::rename(path_ + "." + std::to_string(i), path_ + "." + std::to_string(i + 1), ec);
    if (maxFiles_ > 1) fs::rename(path_, path_ + ".1", ec);
    else fs::remove(path_, ec);
    file_.clear();
    file_.open(path_, std::ios::trunc);
    fileBytes_ = 0;
}

void Logger::write(LogLevel lvl, const std::string& text) {
    static const char* tag[] = {"DEBUG", "INFO ", "WARN ", "ERROR"};
    LogEntry e{nowTimestamp(), lvl, text};
    std::lock_guard<std::mutex> lk(mtx_);
    if (file_.is_open()) {
        if (fileBytes_ >= maxBytes_) rotateLocked();
        std::string line = e.time + " [" + tag[static_cast<int>(lvl)] + "] " + text + '\n';
        file_ << line;
        file_.flush();
        fileBytes_ += line.size();
    }
    buf_.push_back(std::move(e));
    if (buf_.size() > kMax) buf_.pop_front();
    ++gen_;
}

std::vector<LogEntry> Logger::snapshot() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return {buf_.begin(), buf_.end()};
}

std::size_t Logger::generation() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return gen_;
}

void Logger::clear() {
    std::lock_guard<std::mutex> lk(mtx_);
    buf_.clear();
    ++gen_;
}

} // namespace p2p
