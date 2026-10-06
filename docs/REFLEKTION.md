# Individuell reflektion – Alexander Jakobsson

> Mall. Texten under varje rubrik skrivs av mig själv; frågorna är stöd och tas bort före inlämning.

## 1. Sammanfattning av projektet

- Vad gör Solvakt, med egna ord?
- Vad var målet med just den här versionen?

## 2. Processer, trådar och synkronisering (kursmål 1, 7)

- Varför är fetchern en egen process och inte en tråd?
- Vad gör `fork()`, `execl()` och `waitpid()` i `fetcher_proc.c`, och vad händer om `waitpid` utelämnas?
- Hur fungerar kön i `queue.c`? Varför `while` runt `pthread_cond_wait`?
- Vilka resurser delas mellan trådar och hur skyddas de?
- Vad var svårast att få rätt?

## 3. Interprocesskommunikation (kursmål 2, 8)

- Varför pipes mot fetchern och Unix-socket mot klienten?
- Varför måste oanvända pipe-ändar stängas efter `fork()`?
- Hur upptäcker servern att fetchern har dött?
- När hade delat minne varit ett bättre val?

## 4. C jämfört med C++ (kursmål 3)

- Vilka skillnader märktes mellan att skriva servern i C och klienten i C++?
- Felhantering: returkoder mot undantag.
- Resurshantering: `goto fail`-kedjan i `server_create` mot destruktorer.

## 5. Objektmodellen och RAII (kursmål 4, 9)

- Hur fungerar `UnixSocket`? Vad händer med socketen om ett undantag kastas?
- Varför är kopiering borttagen och vad gör move-konstruktorn?
- Varför behöver `Client` ingen egen destruktor?

## 6. STL (kursmål 5, 9)

- Vilka behållare och algoritmer används i klienten och varför?
- Hur hanterar `std::vector` och `std::string` sitt minne?

## 7. Profilering och optimering (kursmål 6, 10, 11)

- Hur gick profileringen till och vad visade gprof?
- Varför hjälpte `-O2` så lite före ändringen och mer efter?
- Vad lärde jag mig om när det lönar sig att optimera?

## 8. Designval jag skulle göra annorlunda

-

## 9. Arbetsprocess och lärdomar

- Hur jag arbetade, vilka hjälpmedel jag använde och hur.
- Vad jag tar med mig till nästa projekt.
