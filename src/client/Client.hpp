#ifndef SOLVAKT_CLIENT_HPP
#define SOLVAKT_CLIENT_HPP

#include "client/UnixSocket.hpp"

#include <ctime>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace solvakt {

// Servern svarade med ERR; meddelandet är serverns egen förklaring.
class ServerError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Svaret följde inte protokollet (fel antal fält, inte ett tal och liknande).
class ProtocolError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct ForecastRow {
    std::time_t time{};
    double radiation{};      // W/m²
    double cloudCover{};     // %
    double temperature{};    // °C
    double productionKwh{};  // per kvart
};

struct PriceRow {
    std::time_t time{};
    double price{};  // SEK/kWh
};

struct PlanRow {
    std::time_t time{};
    std::string action;
    double price{};
    double productionKwh{};
    double consumptionKwh{};
    double batteryKwh{};
    double gridKwh{};  // positivt = köpt, negativt = sålt
};

// Typat gränssnitt mot servern. Varje metod skickar ett kommando och tolkar
// svaret; fel rapporteras som undantag.
class Client {
public:
    explicit Client(const std::string& socketPath);

    std::map<std::string, std::string> status();
    std::vector<ForecastRow> forecast();
    std::vector<PriceRow> prices();
    // hours = 0 ger hela planen.
    std::vector<PlanRow> plan(int hours = 0);

private:
    using Fields = std::vector<std::string>;

    // Skickar kommandot och returnerar svarets rader uppdelade i fält.
    std::vector<Fields> request(const std::string& command);

    UnixSocket socket_;
};

}  // namespace solvakt

#endif
