#include "Stream.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace sdl3mcp {

void StreamBase::open() {
   if (finished_) {
      return;
   }
   if (handler_ != nullptr) {
      handler_->onOpen();
   }
}

void StreamBase::feed(std::string_view text) {
   feed(reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

void StreamBase::feed(const uint8_t* data, size_t size) {
   if (finished_ || size == 0) {
      return;
   }

   size_t offset = 0;
   while (offset < size) {
      if (awaitingPayload()) {
         const size_t missing = payloadExpected_ - payload_.size();
         const size_t taken = std::min(missing, size - offset);
         payload_.insert(payload_.end(), data + offset, data + offset + taken);
         offset += taken;
         if (!awaitingPayload()) {
            std::vector<uint8_t> payload = std::move(payload_);
            payload_.clear();
            payloadExpected_ = 0;
            if (handler_ != nullptr) {
               handler_->onPayload(std::move(payload));
            }
            if (finished_) {
               return;
            }
         }
         continue;
      }

      const char* begin = reinterpret_cast<const char*>(data + offset);
      const void* found = std::memchr(begin, '\n', size - offset);
      if (found == nullptr) {
         line_.append(begin, size - offset);
         if (line_.size() > kMaxLineBytes) {
            failLineTooLong();
            return;
         }
         return;
      }

      const size_t chunk = static_cast<const char*>(found) - begin;
      line_.append(begin, chunk);
      offset += chunk + 1;
      if (line_.size() > kMaxLineBytes) {
         failLineTooLong();
         return;
      }

      std::string line = std::move(line_);
      line_.clear();
      /* One trailing CR is framing, not content: accept CRLF clients. */
      if (!line.empty() && line.back() == '\r') {
         line.pop_back();
      }
      if (handler_ != nullptr) {
         handler_->onMessage(line);
      }
      if (finished_) {
         return;
      }
   }
}

void StreamBase::finish() {
   if (finished_) {
      return;
   }
   if (!line_.empty()) {
      fail("the client closed the connection in the middle of a line");
      return;
   }
   if (awaitingPayload()) {
      fail("the client closed the connection in the middle of a payload");
      return;
   }

   finished_ = true;
   wantsWrite_ = false;
   if (handler_ != nullptr) {
      handler_->onClose();
   }
}

void StreamBase::fail(std::string_view error) {
   if (finished_) {
      return;
   }

   finished_ = true;
   wantsWrite_ = false;
   line_.clear();
   payload_.clear();
   payloadExpected_ = 0;
   if (handler_ != nullptr) {
      handler_->onError(error);
      handler_->onClose();
   }
}

void StreamBase::expectPayload(size_t size) {
   if (finished_) {
      return;
   }
   payloadExpected_ = size;
   payload_.clear();
}

void StreamBase::failLineTooLong() {
   fail("input line exceeds the " + std::to_string(kMaxLineBytes) +
        " byte limit");
}

}  // namespace sdl3mcp
