#include "client/Client.hpp"

#include <algorithm>
#include <iterator>
#include <sstream>

namespace solvakt {

namespace {

constexpr std::size_t kMaxRows = 10000;

std::vector<std::string> split(const std::string& line, char separator) {
    std::vector<std::string> fields;
    std::istringstream stream(line);
    std::string field;
    while (std::getline(stream, field, separator)) {
        fields.push_back(field);
    }
    return fields;
}

double toDouble(const std::string& text) {
    try {
        std::size_t used = 0;
        const double value = std::stod(text, &used);
        if (used != text.size()) {
            throw std::invalid_argument(text);
        }
        return value;
    } catch (const std::exception&) {
        throw ProtocolError("förväntade ett tal men fick '" + text + "'");
    }
}

std::time_t toTime(const std::string& text) {
    try {
        std::size_t used = 0;
        const long long value = std::stoll(text, &used);
        if (used != text.size()) {
            throw std::invalid_argument(text);
        }
        return static_cast<std::time_t>(value);
    } catch (const std::exception&) {
        throw ProtocolError("förväntade en tidsstämpel men fick '" + text + "'");
    }
}

void requireFields(const std::vector<std::string>& fields, std::size_t expected) {
    if (fields.size() != expected) {
        throw ProtocolError("förväntade " + std::to_string(expected) + " fält men fick " +
                            std::to_string(fields.size()));
    }
}

}  // namespace

Client::Client(const std::string& socketPath) : socket_(socketPath) {}

std::vector<Client::Fields> Client::request(const std::string& command) {
    socket_.sendLine(command);

    const std::string header = socket_.readLine();
    if (header.rfind("ERR ", 0) == 0) {
        throw ServerError(header.substr(4));
    }
    if (header.rfind("OK ", 0) != 0) {
        throw ProtocolError("oväntat svarshuvud: '" + header + "'");
    }

    std::size_t rowCount = 0;
    try {
        rowCount = std::stoul(header.substr(3));
    } catch (const std::exception&) {
        throw ProtocolError("ogiltigt radantal i svarshuvudet: '" + header + "'");
    }
    if (rowCount > kMaxRows) {
        throw ProtocolError("orimligt många rader i svaret");
    }

    std::vector<Fields> rows;
    rows.reserve(rowCount);
    for (std::size_t i = 0; i < rowCount; ++i) {
        rows.push_back(split(socket_.readLine(), '\t'));
    }
    return rows;
}

std::map<std::string, std::string> Client::status() {
    std::map<std::string, std::string> result;
    for (const Fields& fields : request("STATUS")) {
        requireFields(fields, 2);
        result[fields[0]] = fields[1];
    }
    return result;
}

std::vector<ForecastRow> Client::forecast() {
    const std::vector<Fields> rows = request("FORECAST");
    std::vector<ForecastRow> result;
    result.reserve(rows.size());
    std::transform(rows.begin(), rows.end(), std::back_inserter(result), [](const Fields& f) {
        requireFields(f, 5);
        return ForecastRow{toTime(f[0]), toDouble(f[1]), toDouble(f[2]), toDouble(f[3]),
                           toDouble(f[4])};
    });
    return result;
}

std::vector<PriceRow> Client::prices() {
    const std::vector<Fields> rows = request("PRICES");
    std::vector<PriceRow> result;
    result.reserve(rows.size());
    std::transform(rows.begin(), rows.end(), std::back_inserter(result), [](const Fields& f) {
        requireFields(f, 2);
        return PriceRow{toTime(f[0]), toDouble(f[1])};
    });
    return result;
}

std::vector<PlanRow> Client::plan(int hours) {
    const std::string command = hours > 0 ? "PLAN " + std::to_string(hours) : "PLAN";
    const std::vector<Fields> rows = request(command);
    std::vector<PlanRow> result;
    result.reserve(rows.size());
    std::transform(rows.begin(), rows.end(), std::back_inserter(result), [](const Fields& f) {
        requireFields(f, 7);
        return PlanRow{toTime(f[0]),   f[1],           toDouble(f[2]), toDouble(f[3]),
                       toDouble(f[4]), toDouble(f[5]), toDouble(f[6])};
    });
    return result;
}

}  // namespace solvakt
