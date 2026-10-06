#ifndef SOLVAKT_UNIX_SOCKET_HPP
#define SOLVAKT_UNIX_SOCKET_HPP

#include <string>

namespace solvakt {

// RAII-ägare av en ansluten Unix domain socket. Konstruktorn ansluter och
// destruktorn stänger, så fildeskriptorn kan inte läcka ens när ett undantag
// kastas. Ägandet är unikt: objektet kan flyttas men inte kopieras.
class UnixSocket {
public:
    // Ansluter till socketfilen. Kastar std::runtime_error om det misslyckas.
    explicit UnixSocket(const std::string& path);
    ~UnixSocket();

    UnixSocket(const UnixSocket&) = delete;
    UnixSocket& operator=(const UnixSocket&) = delete;

    UnixSocket(UnixSocket&& other) noexcept;
    UnixSocket& operator=(UnixSocket&& other) noexcept;

    // Skickar raden följd av radbrytning.
    void sendLine(const std::string& line);

    // Läser fram till nästa radbrytning och returnerar raden utan den.
    // Kastar std::runtime_error vid timeout eller om servern kopplar ned.
    std::string readLine();

    bool isOpen() const noexcept { return fd_ >= 0; }

private:
    void close() noexcept;

    int fd_{-1};
    std::string buffer_;  // mottagna byte som ännu inte lämnats ut
};

}  // namespace solvakt

#endif
