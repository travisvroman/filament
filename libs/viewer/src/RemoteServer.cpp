/*
 * Copyright (C) 2021 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <viewer/RemoteServer.h>

#include <CivetServer.h>

#include <utils/Log.h>

#include <vector>

using namespace utils;

namespace filament {
namespace viewer {

class WebSocketHandler : public CivetWebSocketHandler {
   public:
    WebSocketHandler(RemoteServer* server) : mServer(server) {}

    bool handleConnection(CivetServer* server, const struct mg_connection* conn) override {
        return true;
    }

    void handleReadyState(CivetServer* server, struct mg_connection* conn) override {
        mConnection = conn;
    }

    bool handleData(CivetServer* server, struct mg_connection* conn, int bits, char* data,
                    size_t size) override;

    void handleClose(CivetServer* server, const struct mg_connection* conn) override {
        mConnection = nullptr;
    }

   private:
    RemoteServer* mServer;
    std::vector<char> mChunkedMessage;
    int mChunkedMessageRemaining = 0;
    struct mg_connection* mConnection = nullptr;
};

RemoteServer::RemoteServer(int port) {
    // By default the server spawns 50 threads so we override this to 2. This limits the
    // server to having no more than 2 clients, which is probably fine.
    const char* kServerOptions[] = {
        "listening_ports", "8082",
        "num_threads",     "2",
        "error_log_file",  "civetweb.txt",
        nullptr,
    };
    std::string portString = std::to_string(port);
    kServerOptions[1] = portString.c_str();
    mCivetServer = new CivetServer(kServerOptions);
    if (!mCivetServer->getContext()) {
        delete mCivetServer;
        mCivetServer = nullptr;
        slog.e << "Unable to start RemoteServer, see civetweb.txt for details." << io::endl;
    }

    slog.i << "RemoteServer listening at http://localhost:" << port << io::endl;
    mWebSocketHandler = new WebSocketHandler(this);
    mCivetServer->addWebSocketHandler("", mWebSocketHandler);
}

RemoteServer::~RemoteServer() {
    delete mCivetServer;
    delete mWebSocketHandler;
    for (auto msg : mIncomingMessages) {
        releaseIncomingMessage(msg);
    }
}

IncomingMessage const * RemoteServer::peekIncomingMessage() const {
    std::lock_guard lock(mIncomingMessagesMutex);
    const size_t oldest = mOldestMessageIndex;
    for (auto msg : mIncomingMessages) { if (msg && msg->messageIndex == oldest) return msg; }
    return nullptr;
}

IncomingMessage const * RemoteServer::acquireIncomingMessage() {
    std::lock_guard lock(mIncomingMessagesMutex);
    const size_t oldest = mOldestMessageIndex;
    for (auto& msg : mIncomingMessages) {
        if (msg && msg->messageIndex == oldest) {
            auto result = msg;
            msg = nullptr;
            ++mOldestMessageIndex;
            return result;
        }
    }
    return nullptr;
}

void RemoteServer::enqueueIncomingMessage(IncomingMessage* message) {
    std::lock_guard lock(mIncomingMessagesMutex);
    for (auto& msg : mIncomingMessages) {
        if (!msg) {
            message->messageIndex = mNextMessageIndex++;
            msg = message;
            return;
        }
    }
    slog.e << "Discarding message, message queue overflow." << io::endl;
}

void RemoteServer::releaseIncomingMessage(IncomingMessage* message) {
    if (message) {
        delete[] message->label;
        delete[] message->buffer;
        delete message;
    }
}

// NOTE: This is invoked off the main thread.
bool WebSocketHandler::handleData(CivetServer* server, struct mg_connection* conn, int bits,
                                  char* data, size_t size) {
    const int opcode = bits & 0xf;
    if (opcode == MG_WEBSOCKET_OPCODE_CONNECTION_CLOSE) {
        return true;
    }

    // First check if this chunk is a continuation of a partial existing message.
    if (mChunkedMessageRemaining > 0) {

        // Append the partial existing message.
        mChunkedMessage.insert(mChunkedMessage.end(), data, data + size);

        // Determine number of outstanding bytes.
        if (size > mChunkedMessageRemaining) {
            mChunkedMessageRemaining = 0;
        } else {
            mChunkedMessageRemaining -= size;
        }

        // Return early and wait for more chunks if some bytes are still outstanding.
        if (mChunkedMessageRemaining > 0) {
            return true;
        }

        data = mChunkedMessage.data();
        size = mChunkedMessage.size();
    }

    mChunkedMessageRemaining = 0;

    IncomingMessage* message = new IncomingMessage({});
    message->buffer = new char[size];
    message->bufferByteCount = size;
    memcpy(message->buffer, data, size);

    mServer->enqueueIncomingMessage(message);

    mChunkedMessage.clear();

    return true;
}

} // namespace viewer
} // namespace filament
