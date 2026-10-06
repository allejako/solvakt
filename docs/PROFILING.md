# Solvakt – profilering och optimering

Beräkningssteget (`compute_plan()` i `src/server/compute.c`) profilerades med gprof. En flaskhals hittades och åtgärdades, vilket sänkte tiden per plan från cirka 16 ms till 4 µs.

| | Före | Efter |
|---|---|---|
| Tid per plan, `-O2` | 15 432 µs | 4,0 µs |
| Planer per sekund, `-O2` | 65 | 250 000 |
| Största posten i gprof | `price_percentile`, 100 % | `compute_plan`, 44 % |

## 1. Metod

### Miljö

| | |
|---|---|
| CPU | Intel Core i9-9900K, 3,60 GHz |
| System | Ubuntu 24.04 under WSL2, Linux 6.6.87 |
| Kompilator | GCC 13.3.0 |
| Datum | 2026-10-06 |

### Benchmark

`bench/bench_compute.c` bygger en syntetisk prognos på 192 kvartar (48 timmar) med sol mellan kl. 06 och 18, pristoppar morgon och kväll samt lite brus, och anropar `compute_plan()` ett givet antal gånger. Tiden mäts med `clock_gettime(CLOCK_MONOTONIC)` runt hela loopen.

Programmet skriver också ut en kontrollsumma av resultaten. Den ska vara densamma före och efter en optimering, annars har beteendet ändrats.

```bash
make bench BENCH_N=200          # tid per plan
make profile BENCH_N=200        # bygger med -pg -O0, kör och skriver gprof-rapport
```

Profileringen görs med `-O0` så att varje funktion syns för sig i rapporten och inte försvinner genom inlining.

### Rådata

| Fil | Innehåll |
|---|---|
| [profiling/gprof_before.txt](profiling/gprof_before.txt) | gprof före optimering |
| [profiling/bench_before.txt](profiling/bench_before.txt) | Benchmark före, `-O0` och `-O2` |
| [profiling/gprof_after.txt](profiling/gprof_after.txt) | gprof efter optimering |
| [profiling/bench_after.txt](profiling/bench_after.txt) | Benchmark efter, `-O0` och `-O2` |
| [profiling/valgrind_server.txt](profiling/valgrind_server.txt) | Valgrind på en serverkörning |

## 2. Före optimering

Kodversion: commit `1a0a461`.

### gprof

200 planer, total körtid 3,79 s:

```
  %   cumulative   self              self     total
 time   seconds   seconds    calls  ms/call  ms/call  name
100.00      3.78     3.78    76800     0.05     0.05  price_percentile
  0.00      3.78     0.00    38400     0.00     0.00  compute_production_kwh
  0.00      3.78     0.00     8000     0.00     0.00  min3
  0.00      3.78     0.00      200     0.00    18.90  compute_plan
```

### Tolkning

- All mätbar tid ligger i `price_percentile`. Resten av beräkningen syns inte ens i profilen.
- Funktionen anropas 76 800 gånger för 200 planer, det vill säga 384 gånger per plan. Det är 2 anrop × 192 kvartar: prisgränserna för lågt och högt pris räknades om för varje kvart, trots att de är desamma för hela planen.
- Varje anrop kopierade alla 192 priser och sorterade dem med bubbelsortering, ungefär 192 × 191 / 2 ≈ 18 000 jämförelser. Per plan blir det cirka 7 miljoner jämförelser.

Flaskhalsen är alltså algoritmisk: ett arbete som bara behöver göras en gång per plan gjordes 384 gånger, och med en sorteringsalgoritm som är kvadratisk.

### Benchmark

200 iterationer, tre körningar per nivå:

| Optimeringsnivå | Körning 1 | Körning 2 | Körning 3 | Median |
|---|---|---|---|---|
| `-O0` | 15 933 µs | 15 983 µs | 16 016 µs | 15 983 µs |
| `-O2` | 15 469 µs | 15 404 µs | 15 432 µs | 15 432 µs |

Kontrollsumma: `1815.773881`.

Kompilatoroptimering gav bara 3 % här. En kompilator kan göra en given loop snabbare, men den kan inte se att samma sortering upprepas i onödan.

## 3. Optimeringen

Kodversion: commit `cdc841b`. Ändringen ligger helt i `src/server/compute.c`.

Före (förkortat):

```c
for (int i = 0; i < count; i++)
{
    double low = price_percentile(slots, count, LOW_PERCENTILE);   /* kopierar och bubbelsorterar */
    double high = price_percentile(slots, count, HIGH_PERCENTILE); /* en gång till */
    ...
}
```

Efter:

```c
double low = 0.0, high = 0.0;
price_thresholds(slots, count, &low, &high);   /* en kopia, en qsort */

for (int i = 0; i < count; i++)
{
    ...
}
```

Två ändringar:

1. **Gränserna räknas en gång per plan**, före loopen, och båda percentilerna läses ur samma sorterade kopia.
2. **`qsort` ersätter bubbelsorteringen**, ungefär n·log₂ n ≈ 1 500 jämförelser i stället för 18 000.

Antalet jämförelser per plan går från cirka 7 miljoner till cirka 1 500.

Samtidigt ändrades standardnivån i `Makefile` från `-O0` till `-O2`.

## 4. Efter optimering

### gprof

200 000 planer (tusen gånger fler än före, för att få tillräckligt många mätpunkter):

```
  %   cumulative   self              self     total
 time   seconds   seconds    calls  us/call  us/call  name
 43.62      0.41     0.41   200000     2.05     2.77  compute_plan
 40.96      0.80     0.39                             compare_double
  7.45      0.86     0.07   200000     0.35     0.35  price_thresholds
  5.85      0.92     0.06 38400000     0.00     0.00  compute_production_kwh
  2.13      0.94     0.02  8000000     0.00     0.00  min3
```

### Tolkning

- Tiden är nu fördelad: själva planloopen i `compute_plan` och jämförelsefunktionen som `qsort` anropar tar ungefär lika mycket.
- `price_thresholds` anropas exakt en gång per plan (200 000 anrop för 200 000 planer).
- `compare_double` saknar anropsräknare eftersom den anropas från `qsort` i libc, som inte är byggd med `-pg`. Av samma skäl syns inte tiden inne i `qsort` själv i profilen.
- Ingen enskild funktion dominerar längre på det sätt `price_percentile` gjorde.

### Benchmark

200 000 iterationer, tre körningar per nivå:

| Optimeringsnivå | Körning 1 | Körning 2 | Körning 3 | Median |
|---|---|---|---|---|
| `-O0` | 7,51 µs | 7,38 µs | 7,39 µs | 7,39 µs |
| `-O2` | 4,00 µs | 3,97 µs | 4,07 µs | 4,00 µs |

Kontrollsumma med 200 iterationer: `1815.773881`, samma som före. Planerna är alltså identiska; bara tiden har ändrats. Enhetstesterna i `tests/test_compute.c` passerar oförändrade.

## 5. Jämförelse

| Version | `-O0` | `-O2` |
|---|---|---|
| Före (commit `1a0a461`) | 15 983 µs | 15 432 µs |
| Efter (commit `cdc841b`) | 7,39 µs | 4,00 µs |
| Förbättring | ca 2 200 gånger | ca 3 900 gånger |

| Åtgärd | Effekt |
|---|---|
| Algoritmändringen, vid `-O0` | 15 983 → 7,39 µs |
| `-O2` på den ursprungliga koden | 15 983 → 15 432 µs (3 %) |
| `-O2` på den optimerade koden | 7,39 → 4,00 µs (46 %) |

Slutsatser:

- Profileringen pekade ut exakt en funktion. Utan mätningen hade det varit lätt att i stället börja finslipa planloopen, som visade sig stå för en försumbar del av tiden.
- Den algoritmiska ändringen gav tre tiopotenser; kompilatorflaggan gav som mest en halvering. Kompilatoroptimering lönar sig först när algoritmen är rimlig.
- Efter ändringen är beräkningssteget inte längre en flaskhals i systemet. En plan beräknas var 15:e minut och tar 4 µs, medan HTTP-hämtningen före den tar tiotals till hundratals millisekunder. Fler optimeringar av compute-steget vore därför bortkastat arbete.

## 6. Minnesanalys

### Valgrind

```bash
make valgrind
```

kör alla enhetstester och benchmarken med `valgrind --leak-check=full --error-exitcode=1`. Samtliga passerar utan fel eller läckor.

Servern kördes dessutom under Valgrind i ett helt förlopp: start, hämtning från båda API:erna, beräkning, sex klientanrop varav fem samtidiga, och stopp med `SIGTERM`:

```
HEAP SUMMARY:
    in use at exit: 0 bytes in 0 blocks
  total heap usage: 5,497 allocs, 5,497 frees, 617,946 bytes allocated

All heap blocks were freed -- no leaks are possible

ERROR SUMMARY: 0 errors from 0 contexts (suppressed: 0 from 0)
```

Klienten (`solvakt-client plan 24`) kördes på samma sätt utan fel.

### Minnesanvändning i compute-steget

`compute_plan()` gör inga heap-allokeringar. Den enda arbetsytan är en lokal array med 288 `double` (2,3 kB) på stacken för de sorterade priserna. Indata (`Slot`, 48 byte per kvart) och utdata (`PlanSlot`, 80 byte per kvart) ligger i sammanhängande arrayer som gås igenom i ordning, vilket är gynnsamt för processorns cache. Hela minnesmodellen beskrivs i [ARCHITECTURE.md](ARCHITECTURE.md#7-minnesmodell).
