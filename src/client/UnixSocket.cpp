#include "client/UnixSocket.hpp"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <utility>

#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

namespace solvakt {

namespace {
constexpr int kReceiveTimeoutSeconds = 10;
}

UnixSocket::UnixSocket(const std::string& path) {
    sockaddr_un addr{};
    if (path.empty() || path.size() >= sizeof(addr.sun_path)) {
        throw std::runtime_error("ogiltig socketsökväg: '" + path + "'");
    }
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

    fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd_ < 0) {
        throw std::runtime_error(std::string("socket() misslyckades: ") + std::strerror(errno));
    }

    if (::connect(fd_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        const std::string reason = std::strerror(errno);
        close();  // destruktorn körs inte när konstruktorn kastar
        throw std::runtime_error("kan inte ansluta till " + path + ": " + reason +
                                 " (kör servern?)");
    }

    const timeval timeout{kReceiveTimeoutSeconds, 0};
    ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
}

UnixSocket::~UnixSocket() {
    close();
}

UnixSocket::UnixSocket(UnixSocket&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)), buffer_(std::move(other.buffer_)) {}

UnixSocket& UnixSocket::operator=(UnixSocket&& other) noexcept {
    if (this != &other) {
        close();
        fd_ = std::exchange(other.fd_, -1);
        buffer_ = std::move(other.buffer_);
    }
    return *this;
}

void UnixSocket::close() noexcept {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

void UnixSocket::sendLine(const std::string& line) {
    const std::string data = line + "\n";
    std::size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t n = ::send(fd_, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error(std::string("kunde inte skicka till servern: ") +
                                     std::strerror(errno));
        }
        sent += static_cast<std::size_t>(n);
    }
}

std::string UnixSocket::readLine() {
    for (;;) {
        const std::size_t newline = buffer_.find('\n');
        if (newline != std::string::npos) {
            std::string line = buffer_.substr(0, newline);
            buffer_.erase(0, newline + 1);
            return line;
        }

        char chunk[4096];
        const ssize_t n = ::recv(fd_, chunk, sizeof(chunk), 0);
        if (n == 0) {
            throw std::runtime_error("servern stängde anslutningen");
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                throw std::runtime_error("servern svarade inte i tid");
            }
            throw std::runtime_error(std::string("läsfel från servern: ") + std::strerror(errno));
        }
        buffer_.append(chunk, static_cast<std::size_t>(n));
    }
}

}  // namespace solvakt
