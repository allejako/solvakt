# Solvakt

Lokal plattform för prognos och optimering av solel (LEOP, Kurs 3 – Systemprogrammering och introduktion till C++).

Solvakt hämtar väderprognos och spotpriser i 15-minutersupplösning, beräknar förväntad solcellsproduktion och tar fram en energiplan för 24–48 timmar som för varje kvart säger om el ska köpas, användas direkt från solcellerna, lagras i batteriet, tas ur batteriet eller säljas.

Allt körs lokalt. De enda externa beroendena vid körning är de två öppna API:erna [Open-Meteo](https://open-meteo.com) (väder) och [elprisetjustnu.se](https://www.elprisetjustnu.se) (spotpris).

## Översikt

```
solvakt-server (C)                         solvakt-fetcher (C, egen process)
  fetch-tråd  ── pipe ──▶ GET <url> ──────▶  hämtar med libcurl
              ◀─ pipe ─── OK/ERR + data ───
      │ kö
  parse-tråd      JSON → kvartsintervall med väder + pris
      │ kö
  compute-tråd    solproduktion + energiplan
      │
  cache (TTL)
      ▲
  Unix domain socket, en tråd per klient
      ▲
solvakt-client (C++17)
```

| Binär | Språk | Uppgift |
|---|---|---|
| `bin/solvakt-server` | C | Pipeline fetch → parse → compute → cache, svarar klienter |
| `bin/solvakt-fetcher` | C | Hämtar data över HTTP, startas av servern som barnprocess |
| `bin/solvakt-client` | C++ | Visar prognos, spotpriser och energiplan |

Mer detaljer finns i [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Installation

Utvecklat och testat på Ubuntu 24.04 (WSL2) med GCC 13.

```bash
sudo apt-get install build-essential libcurl4-openssl-dev valgrind
make
```

`valgrind` behövs bara för `make valgrind`. `gprof` följer med `binutils` i `build-essential`.

## Köra

```bash
make run                      # startar servern i förgrunden, Ctrl-C stoppar
```

I en annan terminal:

```bash
bin/solvakt-client status     # finns data, och hur gammal är den?
bin/solvakt-client forecast   # väder och förväntad solproduktion per kvart
bin/solvakt-client prices     # spotpris per kvart
bin/solvakt-client plan       # hela energiplanen
bin/solvakt-client plan 24    # de första 24 timmarna
```

Exempel på utdata från `plan`:

```
Tid                 SEK/kWh  Prod kWh  Förbr kWh Batteri kWh  Nät kWh  Åtgärd
2026-10-06 00:00    -0.0003     0.000      0.250       0.750    1.000  Ladda batteriet
2026-10-06 10:00     0.0596     0.248      0.250      10.000    0.002  Använd egen sol
2026-10-06 10:15     0.0483     0.269      0.250      10.000   -0.019  Sälj överskott
2026-10-06 17:30     1.1984     0.050      0.250      10.000    0.201  Köp från nätet
2026-10-06 17:45     1.2367     0.029      0.250       9.779    0.000  Använd batteriet
```

Servern stoppas kontrollerat med Ctrl-C (SIGINT) eller från en annan terminal:

```bash
make stop                     # skickar SIGTERM
```

Vid stopp avslutas trådarna i ordning, fetcher-processen väntas in och socketfilen tas bort.

Servern kan också startas direkt med en egen konfigurationsfil, och klienten kan peka på en annan socket:

```bash
bin/solvakt-server -c /sökväg/till/min.conf
bin/solvakt-client -s /tmp/annan.sock plan
```

## Konfiguration

Inställningarna ligger i [config/solvakt.conf](config/solvakt.conf) i formatet `nyckel = värde`. Filen är kommenterad rad för rad. Nycklar som utelämnas får standardvärdet. Ogiltiga värden eller okända nycklar gör att servern vägrar starta och skriver ut vilken rad som är fel.

| Nyckel | Standard | Beskrivning |
|---|---|---|
| `socket_path` | `/tmp/solvakt.sock` | Unix-socket för klienter |
| `log_file` | `logs/solvakt.log` | Loggfil; tomt värde loggar bara till stderr |
| `log_level` | `info` | `debug`, `info`, `warn` eller `error` |
| `latitude`, `longitude` | Stockholm | Plats för väderprognosen |
| `price_region` | `SE3` | Elområde, `SE1`–`SE4` |
| `panel_area_m2` | `20` | Solpanelernas yta |
| `panel_efficiency` | `0.18` | Verkningsgrad, 0–1 |
| `base_load_kw` | `1.0` | Hushållets genomsnittliga förbrukning |
| `battery_capacity_kwh` | `10` | Batteriets kapacitet; 0 = inget batteri |
| `battery_max_kw` | `3` | Högsta ladd- och urladdningseffekt |
| `fetch_interval_s` | `900` | Hur ofta ny data hämtas |
| `cache_ttl_s` | `1800` | Hur länge en plan är giltig i cachen |
| `http_timeout_s` | `10` | Timeout per HTTP-anrop |
| `weather_api_url`, `price_api_url` | – | Bas-URL:er för API:erna |

## Tester

```bash
make test        # enhetstester: kö, konfiguration, cache, parser, compute
make valgrind    # samma tester + benchmark under Valgrind
```

Testerna är skrivna i C med ett minimalt eget ramverk ([tests/check.h](tests/check.h)) och har inga externa beroenden. Parsertestet använder riktiga API-svar som sparats i `tests/fixtures/`.

## Profilering

```bash
make bench       # tid per beräknad plan
make profile     # gprof-profil av beräkningssteget
```

Resultat och före/efter-mätningar finns i [docs/PROFILING.md](docs/PROFILING.md).

## Projektstruktur

```
config/solvakt.conf     konfiguration
src/common/             loggning, konfiguration, trådsäker kö, gemensamma typer
src/server/             server, fetcher-hantering, parse, compute, cache
src/fetcher/            fetcher-processen
src/client/             C++-klienten
src/libs/               cJSON (tredjepart)
tests/                  enhetstester och sparade API-svar
bench/                  benchmark för beräkningssteget
docs/                   arkitektur, protokoll, profilering, reflektion
```

## Dokumentation

- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) – processer, trådar, IPC, minnesmodell och designval
- [docs/PROTOCOL.md](docs/PROTOCOL.md) – protokollet mellan klient och server samt mellan server och fetcher
- [docs/PROFILING.md](docs/PROFILING.md) – profilering, flaskhals och optimering med mätningar
- [docs/REFLEKTION.md](docs/REFLEKTION.md) – individuell reflektion

## Tredjepartskod

`src/libs/cJSON.c` och `cJSON.h` är biblioteket [cJSON](https://github.com/DaveGamble/cJSON) 1.7 (MIT-licens) och används för att tolka JSON. Fetchern länkar mot libcurl. All övrig kod är skriven för det här projektet.

## Kända begränsningar

- Planen börjar vid svensk midnatt innevarande dygn och sträcker sig så långt det finns spotpriser: 24 timmar, eller 48 timmar när morgondagens priser har publicerats (ungefär kl. 13).
- Batteriet antas vara tomt vid planens början och modelleras utan förluster.
- Planen är regelbaserad (prisgränser vid 25:e och 75:e percentilen) och räknar på spotpris utan nätavgift, skatt och moms.
- Förbrukningen är ett konstant medelvärde från konfigurationen.
