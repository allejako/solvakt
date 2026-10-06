# Solvakt – protokoll

Systemet har två gränssnitt mellan processer: klient ↔ server över en Unix domain socket, och server ↔ fetcher över två anonyma pipes. Båda är radbaserade textprotokoll i UTF-8.

## 1. Klient ↔ server

| | |
|---|---|
| Transport | Unix domain socket, `SOCK_STREAM` |
| Sökväg | `socket_path` i konfigurationen, standard `/tmp/solvakt.sock` |
| Samtidighet | Servern startar en tråd per ansluten klient |
| Session | Klienten får skicka flera kommandon på samma anslutning |
| Inaktivitet | Servern stänger en anslutning som varit tyst i 30 sekunder |

### Förfrågan

En rad som avslutas med `\n`, högst 127 tecken. Kommandot är inte skiftlägeskänsligt.

| Kommando | Betydelse |
|---|---|
| `STATUS` | Cachens tillstånd |
| `FORECAST` | Väder och förväntad solproduktion per kvart |
| `PRICES` | Spotpris per kvart |
| `PLAN` | Hela energiplanen |
| `PLAN <timmar>` | De första `<timmar>` timmarna av planen, 1–72 |

### Svar

Lyckat svar börjar med en huvudrad som anger hur många datarader som följer:

```
OK <antal rader>\n
<rad 1>\n
<rad 2>\n
...
```

Fält inom en rad skiljs åt med tabb (`\t`). Decimaltal skrivs med punkt. Tider är Unix-tid i sekunder (UTC) och anger kvartens start.

Fel ger en enda rad och inga datarader:

```
ERR <meddelande>\n
```

### Datarader per kommando

**STATUS** – en rad per nyckel: `<nyckel>\t<värde>`

| Nyckel | Värde |
|---|---|
| `state` | `ok`, `no_data` (inget hämtat ännu) eller `expired` (äldre än TTL) |
| `age_s` | Planens ålder i sekunder |
| `ttl_s` | Cachens TTL i sekunder |
| `slots` | Antal kvartar i planen (bara när `state` är `ok`) |
| `from` | Planens starttid (bara när `state` är `ok`) |
| `to` | Planens sluttid (bara när `state` är `ok`) |

**FORECAST** – `<tid>\t<instrålning W/m²>\t<molnighet %>\t<temperatur °C>\t<produktion kWh>`

**PRICES** – `<tid>\t<pris SEK/kWh>`

**PLAN** – `<tid>\t<åtgärd>\t<pris SEK/kWh>\t<produktion kWh>\t<förbrukning kWh>\t<batteri kWh>\t<nät kWh>`

| Fält | Betydelse |
|---|---|
| åtgärd | `BUY`, `USE_SOLAR`, `CHARGE`, `DISCHARGE` eller `SELL` |
| produktion, förbrukning | Energi under kvarten |
| batteri | Batteriets laddning efter kvarten |
| nät | Positivt = köpt från nätet, negativt = sålt till nätet |

| Åtgärd | När |
|---|---|
| `BUY` | Underskott som köps från nätet |
| `USE_SOLAR` | Solen täcker minst hälften av förbrukningen, resten köps |
| `CHARGE` | Batteriet laddas, av solöverskott eller av billig el från nätet |
| `DISCHARGE` | Dyr el: underskottet tas ur batteriet |
| `SELL` | Solöverskott säljs (högt pris eller fullt batteri) |

### Felmeddelanden

| Meddelande | Orsak |
|---|---|
| `tomt kommando` | Tom rad |
| `okänt kommando, …` | Kommandot finns inte |
| `för många argument` | Fler argument än kommandot tar |
| `antal timmar måste vara 1-72` | Ogiltigt argument till `PLAN` |
| `ingen data ännu, …` | Servern har inte hunnit hämta data |
| `datat är för gammalt, …` | Cachens TTL har gått ut eftersom hämtningen misslyckas |

En rad som är längre än 127 tecken gör att servern stänger anslutningen utan svar.

### Exempel

```
→ PLAN 1
← OK 4
← 1791237600	CHARGE	-0.00034	0.0000	0.2500	0.7500	1.0000
← 1791238500	CHARGE	-0.00022	0.0000	0.2500	1.5000	1.0000
← 1791239400	CHARGE	-0.00045	0.0000	0.2500	2.2500	1.0000
← 1791240300	CHARGE	-0.00045	0.0000	0.2500	3.0000	1.0000

→ PLAN 99
← ERR antal timmar måste vara 1-72
```

Protokollet går att prova utan klienten:

```bash
echo STATUS | nc -U /tmp/solvakt.sock
```

## 2. Server ↔ fetcher

| | |
|---|---|
| Transport | Två anonyma pipes, kopplade till fetcherns stdin och stdout med `dup2()` |
| Riktning | Servern frågar, fetchern svarar; en förfrågan i taget |
| Avslut | Servern stänger sin skrivände; fetchern får EOF på stdin och avslutar |

### Förfrågan

```
GET <url>\n
```

Bara `http` och `https` tillåts.

### Svar

Lyckad hämtning: en huvudrad med längden, direkt följd av exakt så många byte data (ingen avslutande radbrytning).

```
OK <antal byte>\n
<data>
```

Misslyckad hämtning:

```
ERR <meddelande>\n
```

`<meddelande>` är `HTTP <statuskod>` för HTTP-svar ≥ 400, annars libcurls felbeskrivning (till exempel timeout eller misslyckad anslutning).

### Felhantering på serversidan

| Händelse | Serverns reaktion |
|---|---|
| `ERR`-svar | Loggas som varning, nytt försök efter högst 60 sekunder; cachen behåller senaste plan tills TTL går ut |
| `ERR` för morgondagens priser | Normalt före ca kl. 13; planen byggs på innevarande dygn |
| EOF eller `EPIPE` på pipen | Fetchern räknas som död, väntas in med `waitpid()` och startas om |
| Inget svar inom `http_timeout_s` + 5 s | Som ovan; strömmen går inte längre att lita på |
| Ogiltig huvudrad eller över 4 MB data | Som ovan |
