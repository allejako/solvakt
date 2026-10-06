// solvakt-client - kommandoradsklient för Solvakt.
//
//   solvakt-client [-s socket] status
//   solvakt-client [-s socket] forecast
//   solvakt-client [-s socket] prices
//   solvakt-client [-s socket] plan [timmar]

#include "client/Client.hpp"

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <numeric>
#include <string>
#include <vector>

using solvakt::Client;
using solvakt::ForecastRow;
using solvakt::PlanRow;
using solvakt::PriceRow;

namespace {

const char* const kDefaultSocket = "/tmp/solvakt.sock";

std::string formatTime(std::time_t time) {
    std::tm local{};
    localtime_r(&time, &local);
    char text[32];
    std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M", &local);
    return text;
}

void printUsage(std::ostream& out) {
    out << "Användning: solvakt-client [-s socket] <kommando>\n\n"
           "Kommandon:\n"
           "  status          serverns tillstånd och hur gammal datan är\n"
           "  forecast        väder och förväntad solproduktion per kvart\n"
           "  prices          spotpris per kvart\n"
           "  plan [timmar]   energiplan; utan argument visas hela planen\n\n"
           "Socketen kan också anges med miljövariabeln SOLVAKT_SOCKET\n"
           "(standard: "
        << kDefaultSocket << ").\n";
}

void requireNoArguments(const std::vector<std::string>& args) {
    if (!args.empty()) {
        throw std::invalid_argument("kommandot tar inga argument");
    }
}

void runStatus(Client& client, const std::vector<std::string>& args) {
    requireNoArguments(args);
    const std::map<std::string, std::string> status = client.status();

    const std::map<std::string, std::string> stateText = {
        {"ok", "data finns och är aktuell"},
        {"no_data", "ingen data har hämtats ännu"},
        {"expired", "datan är äldre än cachens TTL"},
    };

    for (const auto& [key, value] : status) {
        std::string shown = value;
        if (key == "state") {
            const auto found = stateText.find(value);
            if (found != stateText.end()) {
                shown += " (" + found->second + ")";
            }
        } else if (key == "from" || key == "to") {
            shown = formatTime(static_cast<std::time_t>(std::stoll(value)));
        }
        std::cout << std::left << std::setw(8) << key << shown << "\n";
    }
}

void runForecast(Client& client, const std::vector<std::string>& args) {
    requireNoArguments(args);
    const std::vector<ForecastRow> rows = client.forecast();
    if (rows.empty()) {
        std::cout << "Ingen prognos tillgänglig.\n";
        return;
    }

    std::cout << std::left << std::setw(18) << "Tid" << std::right << std::setw(10) << "Sol W/m2"
              << std::setw(9) << "Moln %" << std::setw(9) << "Temp C" << std::setw(12)
              << "Prod kWh" << "\n";
    std::cout << std::fixed;
    for (const ForecastRow& row : rows) {
        std::cout << std::left << std::setw(18) << formatTime(row.time) << std::right
                  << std::setw(10) << std::setprecision(0) << row.radiation << std::setw(9)
                  << row.cloudCover << std::setw(9) << std::setprecision(1) << row.temperature
                  << std::setw(12) << std::setprecision(3) << row.productionKwh << "\n";
    }

    const double total = std::accumulate(
        rows.begin(), rows.end(), 0.0,
        [](double sum, const ForecastRow& row) { return sum + row.productionKwh; });
    const auto peak = std::max_element(
        rows.begin(), rows.end(), [](const ForecastRow& a, const ForecastRow& b) {
            return a.productionKwh < b.productionKwh;
        });

    std::cout << "\nFörväntad produktion totalt: " << std::setprecision(1) << total << " kWh\n"
              << "Högsta kvart: " << std::setprecision(3) << peak->productionKwh << " kWh ("
              << formatTime(peak->time) << ")\n";
}

void runPrices(Client& client, const std::vector<std::string>& args) {
    requireNoArguments(args);
    const std::vector<PriceRow> rows = client.prices();
    if (rows.empty()) {
        std::cout << "Inga priser tillgängliga.\n";
        return;
    }

    std::cout << std::left << std::setw(18) << "Tid" << std::right << std::setw(10) << "SEK/kWh"
              << "\n"
              << std::fixed << std::setprecision(4);
    for (const PriceRow& row : rows) {
        std::cout << std::left << std::setw(18) << formatTime(row.time) << std::right
                  << std::setw(10) << row.price << "\n";
    }

    const double sum = std::accumulate(
        rows.begin(), rows.end(), 0.0,
        [](double total, const PriceRow& row) { return total + row.price; });

    // Sortera en kopia på pris för att hitta de billigaste kvartarna.
    std::vector<PriceRow> byPrice = rows;
    std::sort(byPrice.begin(), byPrice.end(),
              [](const PriceRow& a, const PriceRow& b) { return a.price < b.price; });

    std::cout << "\nMedel: " << sum / static_cast<double>(rows.size()) << " SEK/kWh\n"
              << "Lägst: " << byPrice.front().price << " (" << formatTime(byPrice.front().time)
              << ")\n"
              << "Högst: " << byPrice.back().price << " (" << formatTime(byPrice.back().time)
              << ")\n";
}

void runPlan(Client& client, const std::vector<std::string>& args) {
    int hours = 0;
    if (args.size() > 1) {
        throw std::invalid_argument("plan tar högst ett argument");
    }
    if (args.size() == 1) {
        std::size_t used = 0;
        try {
            hours = std::stoi(args[0], &used);
        } catch (const std::exception&) {
            used = 0;
        }
        if (used != args[0].size() || hours < 1) {
            throw std::invalid_argument("antal timmar måste vara ett positivt heltal");
        }
    }

    const std::vector<PlanRow> rows = client.plan(hours);
    if (rows.empty()) {
        std::cout << "Ingen plan tillgänglig.\n";
        return;
    }

    const std::map<std::string, std::string> actionText = {
        {"BUY", "Köp från nätet"},        {"USE_SOLAR", "Använd egen sol"},
        {"CHARGE", "Ladda batteriet"},    {"DISCHARGE", "Använd batteriet"},
        {"SELL", "Sälj överskott"},
    };

    // setw räknar byte, inte tecken, så åtgärden skrivs sist på raden där
    // svenska tecken inte kan förskjuta kolumnerna.
    std::cout << std::left << std::setw(18) << "Tid" << std::right << std::setw(9) << "SEK/kWh"
              << std::setw(10) << "Prod kWh" << std::setw(12) << "Förbr kWh" << std::setw(12)
              << "Batteri kWh" << std::setw(10) << "Nät kWh"
              << "  Åtgärd\n"
              << std::fixed;

    std::map<std::string, int> quarters;
    double boughtKwh = 0.0, boughtCost = 0.0, soldKwh = 0.0, soldIncome = 0.0;
    for (const PlanRow& row : rows) {
        const auto text = actionText.find(row.action);
        std::cout << std::left << std::setw(18) << formatTime(row.time) << std::right
                  << std::setw(9) << std::setprecision(4) << row.price << std::setw(10)
                  << std::setprecision(3) << row.productionKwh << std::setw(11)
                  << row.consumptionKwh << std::setw(12) << row.batteryKwh << std::setw(9)
                  << row.gridKwh << "  "
                  << (text != actionText.end() ? text->second : row.action) << "\n";

        ++quarters[row.action];
        if (row.gridKwh > 0.0) {
            boughtKwh += row.gridKwh;
            boughtCost += row.gridKwh * row.price;
        } else {
            soldKwh -= row.gridKwh;
            soldIncome -= row.gridKwh * row.price;
        }
    }

    std::cout << "\nSammanfattning (" << rows.size() / 4 << " timmar, " << rows.size()
              << " kvartar)\n";
    for (const auto& [action, count] : quarters) {
        const auto text = actionText.find(action);
        std::cout << "  " << (text != actionText.end() ? text->second : action) << ": " << count
                  << " kvartar\n";
    }
    std::cout << std::setprecision(2) << "  Köpt från nätet: " << boughtKwh << " kWh för "
              << boughtCost << " SEK (spotpris)\n"
              << "  Sålt till nätet: " << soldKwh << " kWh för " << soldIncome << " SEK\n";
}

}  // namespace

int main(int argc, char** argv) {
    // Tiderna gäller svenska elområden; visa svensk tid om inget annat är valt.
    setenv("TZ", "Europe/Stockholm", 0);
    tzset();

    std::vector<std::string> args(argv + 1, argv + argc);

    const char* fromEnv = std::getenv("SOLVAKT_SOCKET");
    std::string socketPath = fromEnv != nullptr ? fromEnv : kDefaultSocket;
    if (args.size() >= 2 && args[0] == "-s") {
        socketPath = args[1];
        args.erase(args.begin(), args.begin() + 2);
    }

    if (args.empty() || args[0] == "-h" || args[0] == "--help") {
        printUsage(args.empty() ? std::cerr : std::cout);
        return args.empty() ? 2 : 0;
    }

    using Handler = std::function<void(Client&, const std::vector<std::string>&)>;
    const std::map<std::string, Handler> commands = {
        {"status", runStatus},
        {"forecast", runForecast},
        {"prices", runPrices},
        {"plan", runPlan},
    };

    const std::string command = args[0];
    const auto handler = commands.find(command);
    if (handler == commands.end()) {
        std::cerr << "Okänt kommando: " << command << "\n\n";
        printUsage(std::cerr);
        return 2;
    }
    args.erase(args.begin());

    try {
        Client client(socketPath);
        handler->second(client, args);
    } catch (const solvakt::ServerError& e) {
        std::cerr << "Servern svarade med fel: " << e.what() << "\n";
        return 1;
    } catch (const std::invalid_argument& e) {
        std::cerr << "Fel användning: " << e.what() << "\n";
        return 2;
    } catch (const std::exception& e) {
        std::cerr << "Fel: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
