#include "logger.h"

#include <string.h>

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

namespace logger {

inline size_t alignUp(size_t x, size_t align) {
    return (x + align - 1) & ~(align - 1);
}

inline size_t alignStringSize(size_t size) {
    return alignUp(size + 1 + SimpleStringRef::headerSize(),
                   SimpleStringRef::alignSize());
}

SimpleStringRef::SimpleStringRef(const char* str, size_t size) : size_(size) {
    if (size == 0) {
        memset(reinterpret_cast<char*>(this) + SimpleStringRef::headerSize(), 0,
               SimpleStringRef::alignSize());
    } else {
        memcpy(reinterpret_cast<char*>(this) + SimpleStringRef::headerSize(),
               str, objSize() - SimpleStringRef::headerSize());
    }
}

size_t SimpleStringRef::objSize() const { return alignStringSize(size_); }

size_t SimpleStringRef::invalidSize() const {
    return SimpleStringRef::headerSize() + size_ + 1;
}

SimpleStringRef* SimpleStringRef::create(StringPool& pool, const char* str,
                                         size_t size) {
    auto buf = pool.allocStringBuf();
    auto strRef = new (buf) logger::SimpleStringRef(str, size);
    assert(pool.pool_begin() <= buf && buf < pool.pool_end() &&
           "buf not in range!");
    return strRef;
}

LogConfig gLogConfig;

#define CHECK_BEGIN_IN_RANGE()                                 \
    assert(pool_begin() <= reinterpret_cast<char*>(front()) && \
           reinterpret_cast<char*>(front()) < pool_end() &&    \
           "begin not in pool range!")

StringPool::StringRefIterator StringPool::StringRefIterator::operator++() {
    ptr_ = ptr_->next();
    return ptr_;
}
StringPool::StringRefIterator StringPool::StringRefIterator::operator++(int) {
    auto cur = ptr_;
    ptr_ = ptr_->next();
    return cur;
}

StringPool::StringPool(const FlushFunc& flush)
    : flushFunc_(flush), pageSize_(gLogConfig.pageSize) {
    pool_ = static_cast<char*>(malloc(pageSize_));
    currentPoolBegin_ = pool_;
    currentPoolEnd_ = currentPoolBegin_ + pageSize_;
    begin_ = reinterpret_cast<SimpleStringRef*>(pool_);
}

StringPool::~StringPool() { free(pool_); }

char* StringPool::allocStringBuf() { return currentPoolBegin_; }

void StringPool::flushPool() {
#if 0
    auto iter = this->begin();
    for (; iter != this->end();) {
        auto preIter = iter++;
        memset(reinterpret_cast<char*>(&*preIter), '*', SimpleStringRef::headerSize());
    }
    flushFunc_(pool_, pageSize_);
#else
    for (auto& curStr : *this) {
        assert(strlen(curStr.c_str()) < pageSize_ && "ilegal str!");
        assert(curStr.c_str() && "flush null string!");
        flushFunc_(curStr.c_str(), curStr.size());
    }
#endif
}

void StringPool::push_back(const std::string& str) {
    size_t alignSize = alignStringSize(str.size());
    if (currentPoolBegin_ + alignSize > currentPoolEnd_) {
        flushPool();
        currentPoolBegin_ = pool_;
        assert(currentPoolBegin_ == pool_begin() && "flush reset error!");
        size_ = 1;
        begin_ = reinterpret_cast<SimpleStringRef*>(pool_);
    } else {
        ++size_;
    }
    (void)SimpleStringRef::create(*this, str.c_str(), str.size());
    currentPoolBegin_ += alignSize;
    assert(size() == debugSize() && "error size!");
}

void StringPool::pop_front() {
    --size_;
    ++begin_;
    CHECK_BEGIN_IN_RANGE();
}

bool StringPool::hasEnoughSpace(size_t size) {
    size_t asize = alignStringSize(size);
    return currentPoolBegin_ + asize <= currentPoolEnd_;
}

size_t StringPool::debugSize() {
    CHECK_BEGIN_IN_RANGE();
    size_t i = 0;
    auto iter = begin();
    while (iter != end()) {
        ++iter;
        ++i;
    }
    assert(reinterpret_cast<char*>(&*iter) < pool_begin() + kPoolPageSize &&
           "offset error!");
    return i;
}

using StlDeque = std::deque<std::string>;

#ifdef USE_STL_QUEUE
using StringQueue = StlDeque;
#else
using StringQueue = StringPool;
#endif

inline void fwriteString(const std::string& str, std::FILE* fh) {
    fwrite(str.c_str(), str.size(), 1, fh);
}

inline void fwriteString(SimpleStringRef* str, std::FILE* fh) {
    fwrite(str->c_str(), str->size(), 1, fh);
}

class LogConsumer : public std::enable_shared_from_this<LogConsumer> {
   public:
    LogConsumer() : exit_(false) {
        tmpBuffer_.resize(256);
        th_ = std::make_unique<std::thread>(&LogConsumer::print, this);
    }

    void pushLog(std::stringstream& ss) {
        auto str = ss.str();
        {
            std::lock_guard<std::mutex> guard(mtx_);
            buf_.push_back(std::move(str));
        }
        ss.clear();
        ss.str("");
    }

    void print() {
        while (!exit_ || buf_.size()) {
            if (buf_.empty()) {
                goto LOOP_END;
            } else {
#if 1
                mtx_.lock();
                if (buf_.empty()) {
                    mtx_.unlock();
                    goto LOOP_END;
                }

#ifdef USE_STL_QUEUE
                auto str = std::move(buf_.front());
#else
                auto str = buf_.front();
#endif

                buf_.pop_front();
                mtx_.unlock();
                fwriteString(str, gLogConfig.stream);
#else
                mtx_.lock();
                size_t consumeSize = buf_.size() <= tmpBuffer_.size()
                                         ? buf_.size()
                                         : tmpBuffer_.size();
                std::copy(buf_.begin(), buf_.begin() + consumeSize,
                          tmpBuffer_.begin());
                for (size_t i = 0; i < consumeSize; ++i) {
                    buf_.pop_front();
                }
                mtx_.unlock();
                for (size_t i = 0; i < consumeSize; ++i) {
                    fwriteString(tmpBuffer_[i].c_str(), tmpBuffer_[i].size(),
                                 gLogConfig.stream);
                }
#endif
            }
        LOOP_END:
#if __cplusplus > 201402L
            auto self = this->weak_from_this();
            if (self.use_count() != 1) {
                std::this_thread::yield();
            }
#else
            auto self = this->shared_from_this();
            if (self.use_count() != 2) {
                std::this_thread::yield();
            }
#endif
        }
    }

    ~LogConsumer() {
        exit_.store(true);
        th_->detach();
    }

    StringQueue& queue() { return buf_; }

   private:
    std::mutex mtx_;
    StringQueue buf_;
    std::atomic<bool> exit_;
    std::unique_ptr<std::thread> th_;
    std::vector<std::string> tmpBuffer_;
};

size_t StringLiteralBase::MN = 0;

LogStream& LogStream::instance() {
    static std::shared_ptr<LogConsumer> gLogConsumer =
        std::make_shared<LogConsumer>();
    // static thread_local std::unique_ptr<LogStream> __instance =
    //     std::make_unique<LogStream>(gLogConsumer);

    static thread_local LogStream* __instance = new LogStream(gLogConsumer);
    return *__instance;
}

std::thread::id LogStream::threadId() {
    static thread_local std::thread::id _id = std::this_thread::get_id();
    return _id;
}

LogStream::LogStream(std::shared_ptr<LogConsumer>& logConsumer)
    : logConsumer_(logConsumer) {
    auto strLevel = std::getenv("LOG_LEVEL");
    if (strLevel) {
        level_ = static_cast<LogLevel>(atoi(strLevel));
    }
}

LogStream::~LogStream() {
    std::stringstream ss;
    ss << LogStream::threadId();
    int64_t tid;
    ss >> tid;
}

void LogStream::flush() {
    ss_ << "\n";
    if (gLogConfig.mode == LogConfig::kSync) {
        fwriteString(ss_.str(), gLogConfig.stream);
    } else {
        logConsumer_->pushLog(ss_);
    }
}

void initLogger(const LogConfig& cfg) {
    gLogConfig = cfg;
    (void)LogStream::instance();
}

thread_local std::chrono::high_resolution_clock::duration
    LogWrapper::totalDur{};

}  // namespace logger