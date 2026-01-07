// Modern C++17 SO Loader - SLEB128 Decoder
#pragma once

#include <cstdint>
#include <cstddef>

namespace soloader {

class Sleb128Decoder {
public:
    Sleb128Decoder(const uint8_t* data, size_t size)
        : current_(data), end_(data + size) {}
    
    int64_t decode() {
        int64_t value = 0;
        size_t shift = 0;
        uint8_t byte;
        
        do {
            if (current_ >= end_) {
                return 0; // 错误处理
            }
            byte = *current_++;
            value |= static_cast<int64_t>(byte & 0x7F) << shift;
            shift += 7;
        } while (byte & 0x80);
        
        // 符号扩展
        if (shift < 64 && (byte & 0x40)) {
            value |= -(static_cast<int64_t>(1) << shift);
        }
        
        return value;
    }
    
    uint64_t decodeUnsigned() {
        uint64_t value = 0;
        size_t shift = 0;
        uint8_t byte;
        
        do {
            if (current_ >= end_) return 0;
            byte = *current_++;
            value |= static_cast<uint64_t>(byte & 0x7F) << shift;
            shift += 7;
        } while (byte & 0x80);
        
        return value;
    }
    
    bool hasMore() const { return current_ < end_; }
    const uint8_t* current() const { return current_; }

private:
    const uint8_t* current_;
    const uint8_t* end_;
};

} // namespace soloader
