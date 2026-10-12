# Individuell reflektion – Alexander Jakobsson

## 1. Sammanfattning av projektet

Solvakt är min individuella lösning på LEOP-uppgiften. Systemet hämtar väderprognos från Open-Meteo och spotpriser från elprisetjustnu.se, räknar ut hur mycket solcellerna väntas producera varje kvart och tar fram en plan som för varje kvart säger om el ska köpas, användas direkt, lagras i batteriet, tas ur batteriet eller säljas.

Jag var med i början av grupprojektet men frånvarande i slutet, och gör därför en egen inlämning. Målet med den här versionen var att uppfylla alla obligatoriska krav med en så liten och begriplig lösning som möjligt. Jag har medvetet valt bort stretch goals: hellre ett system där jag kan förklara varje del än ett större som jag inte har överblick över.

## 2. Processer, trådar och synkronisering (kursmål 1, 7)

Servern består av en huvudtråd som tar emot klienter, tre pipeline-trådar (fetch, parse, compute) och en tråd per ansluten klient. Hämtningen över HTTP ligger i en egen process, `solvakt-fetcher`.

Fetchern är en process och inte en tråd eftersom nätverksanrop är den del som oftast går fel. En tråd delar adressrymd med resten av servern, så en krasch i libcurl hade tagit ned allt. En process har eget minne, och om den dör eller hänger sig kan servern starta en ny och under tiden fortsätta svara klienter ur cachen.

I `fetcher_proc.c` skapar `fork()` en kopia av serverprocessen. I barnet byter `execl()` ut programmet mot fetchern, men behåller de fildeskriptorer som kopplats till stdin och stdout med `dup2()`. `waitpid()` hämtar barnets avslutsstatus. Utan det anropet ligger det avslutade barnet kvar som zombie i processtabellen tills servern själv avslutas. Fetch-tråden anropar därför `waitpid` med `WNOHANG` var 200:e millisekund, så att en död fetcher upptäcks och ersätts utan att tråden blockeras.

Mellan pipeline-stegen ligger en kö (`queue.c`): en ringbuffert med fast storlek, skyddad av en mutex och två villkorsvariabler, en för "inte tom" och en för "inte full". `pthread_cond_wait` släpper låset medan tråden sover och tar det igen när den vaknar. Väntan ligger i en `while`-loop eftersom tråden kan väckas utan att villkoret gäller, antingen av en falsk väckning eller för att en annan tråd hann före. Med `if` hade en konsument kunnat läsa ur en tom kö.

De delade resurserna är köerna, cachen, loggfilen, räknaren över klienttrådar och flaggan `running`. De fyra första har var sin mutex, och `running` är en `atomic_bool`. Ingen tråd håller mer än ett lås åt gången, så deadlock genom fel låsordning kan inte uppstå. Cachen lämnar ut en kopia av planen i stället för en pekare, så låset hålls bara under kopieringen och en långsam klient kan inte blockera compute-tråden.

Det som krävde mest eftertanke var nedstängningen. Trådarna väntar på olika saker: fetch-tråden på nästa hämtning, parse och compute i `queue_pop`, huvudtråden i `accept`. Alla måste väckas, och i rätt ordning. Lösningen blev att `queue_close()` väcker alla som väntar på kön, att `accept`-loopen använder `poll()` med timeout, och att signalhanteraren bara sätter en flagga. Ett konkret misstag på vägen var att servern loggade medan den höll klientlåset under nedstängningen. Det gav inget fel i praktiken, men ett lås ska hållas så kort som möjligt och inte runt I/O, så loggningen flyttades ut.

## 3. Interprocesskommunikation (kursmål 2, 8)

Jag använder två sorters IPC. Mot fetchern används två anonyma pipes, en i varje riktning. De passar eftersom fetchern alltid är serverns barn: pipes ärvs vid `fork()` och behöver inget namn i filsystemet. Mot klienten används en Unix domain socket, eftersom klienten är en orelaterad process som startas senare och måste kunna hitta servern via en sökväg. En socket ger dessutom en egen anslutning per klient, vilket en pipe inte gör.

Efter `fork()` har både förälder och barn alla fyra pipe-ändarna öppna. De som inte används måste stängas, annars kommer EOF aldrig fram. En läsare får EOF först när alla skrivändar är stängda, så om servern hade behållit sin kopia av skrivänden till svarspipen hade den väntat för evigt på en fetcher som redan dött.

Servern upptäcker en död fetcher på två sätt: `waitpid` med `WNOHANG` i fetch-trådens vänteloop, och EOF eller timeout när den läser ett svar. `SIGPIPE` ignoreras, så en skrivning till en stängd pipe ger felkoden `EPIPE` i stället för att döda servern.

Fetcherns svar har ett längdprefix eftersom JSON kan innehålla radbrytningar. Klientprotokollet är däremot radbaserad text med tabbar, vilket är lätt att tolka och går att felsöka för hand.

Delat minne hade varit ett bättre val om många processer behövt läsa stora mängder data ofta, till exempel om cachen skulle delas mellan flera serverprocesser. Då slipper man kopiera data genom kärnan. Priset är att man själv måste synkronisera med semaforer. Här är planen 23 kB och läses sällan, så det hade bara gjort lösningen mer komplicerad.

## 4. C jämfört med C++ (kursmål 3)

Servern är skriven i C och klienten i C++, och skillnaden märks mest i hur resurser och fel hanteras.

I C returnerar funktionerna felkoder som anroparen måste kontrollera, och städningen görs för hand. `server_create` är ett tydligt exempel: den skapar köer, lyssnande socket och fetcher i tur och ordning, och om ett steg misslyckas hoppar den med `goto` till en kedja av etiketter som river det som redan hunnit skapas, i omvänd ordning. Det fungerar, men det är jag som måste hålla ordningen rätt.

I C++ kastar klienten undantag i stället. De fångas på ett ställe i `main()` och översätts till ett felmeddelande och en returkod. Städningen sköts av destruktorer, som körs automatiskt när ett objekt lämnar sitt scope, även när ett undantag kastas. Det som i C är en `goto`-kedja blir i C++ ingen kod alls.

Andra skillnader jag använde: `std::string` i stället för `char`-buffertar med fast längd, referenser och `const` i stället för pekare, namespace (`solvakt`) i stället för namnprefix, och klasser som håller ihop data med de funktioner som arbetar på den. C++ ligger på en högre abstraktionsnivå, men C gör det tydligare exakt vad som händer mot operativsystemet, och därför passade C bättre för servern.

## 5. Objektmodellen och RAII (kursmål 4, 9)

RAII betyder att en resurs ägs av ett objekt: konstruktorn tar resursen och destruktorn lämnar tillbaka den. `UnixSocket` äger en fildeskriptor på det sättet. Konstruktorn ansluter till servern och kastar `std::runtime_error` om det misslyckas, och destruktorn stänger. Om ett undantag kastas mitt i ett kommando rullas stacken upp, destruktorn körs och socketen stängs ändå. Det finns ingen kodväg där den läcker.

Kopiering är borttagen med `= delete`. Två objekt med samma fildeskriptor hade båda försökt stänga den, och den andra stängningen hade i värsta fall träffat en helt annan fil som fått samma nummer. Flytt är däremot tillåten: move-konstruktorn tar över deskriptorn med `std::exchange` och sätter källans `fd_` till -1, så att källans destruktor inte stänger något. Tillsammans med destruktorn och move-tilldelningen är det Rule of Five.

`Client` har en `UnixSocket` som medlem och inga egna resurser. Kompilatorns genererade destruktor anropar medlemmarnas destruktorer, så `Client` behöver ingen egen (Rule of Zero). Medlemmarna är privata och klassen visar bara upp typade metoder som `plan()` och `prices()`, så resten av programmet behöver inte känna till protokollet.

## 6. STL (kursmål 5, 9)

Klienten har inga `new`, `delete`, `malloc` eller `free`. Allt dynamiskt minne ägs av STL-behållare:

- `std::string` för all text.
- `std::vector` för svarsraderna, eftersom antalet rader inte är känt i förväg och de läses i ordning.
- `std::map` för statusnycklar, för kommandotabellen och för att räkna kvartar per åtgärd.

Algoritmerna jag använder är `std::transform` för att göra om textfält till typade rader, `std::accumulate` för summor, `std::max_element` för kvarten med högst produktion och `std::sort` för att hitta billigaste och dyraste pris. De säger vad som görs tydligare än en handskriven loop.

`std::vector` och `std::string` är själva RAII-typer. De äger ett heap-block, växer genom att allokera ett större block och flytta över innehållet, och frigör blocket i sin destruktor. När en vektor returneras från en funktion flyttas den, så bara pekaren byter ägare. `std::unique_ptr` fungerar enligt samma princip för ett enstaka objekt: unikt ägande, går att flytta men inte kopiera, och frigör i destruktorn. Jag behövde ingen i klienten eftersom behållarna redan äger allt, och `UnixSocket` är i praktiken samma idé för en fildeskriptor.

## 7. Profilering och optimering (kursmål 6, 10, 11)

Jag profilerade beräkningssteget med gprof. Ett benchmarkprogram anropar `compute_plan()` många gånger på en syntetisk prognos på 48 timmar, och bygget görs med `-pg -O0` så att varje funktion syns för sig i rapporten.

Resultatet var tydligt: 100 % av tiden låg i `price_percentile`, som anropades 384 gånger per plan. Prisgränserna för lågt och högt pris räknades om för varje kvart trots att de är desamma för hela planen, och varje anrop kopierade och bubbelsorterade alla priser. Jag flyttade ut beräkningen före loopen och bytte till `qsort`. Tiden per plan gick från cirka 15,4 ms till 4 µs. En kontrollsumma i benchmarken visar att planerna är identiska före och efter.

`-O2` gav bara 3 % på den ursprungliga koden men 46 % på den optimerade. Kompilatorn kan göra en given loop snabbare, men den kan inte se att samma sortering upprepas i onödan. Först när det onödiga arbetet var borta fanns det något kvar för kompilatorn att förbättra.

Det jag tar med mig är att mäta först. Jag hade annars lätt kunnat börja putsa på själva planloopen, som stod för en försumbar del av tiden. Lika viktigt är att veta när man ska sluta: en plan beräknas var 15:e minut och tar 4 µs, medan HTTP-hämtningen tar tiotals millisekunder. Mer optimering av compute-steget hade inte märkts.

## 8. Designval jag skulle göra annorlunda

- **Planen är regelbaserad.** Den jämför varje kvarts pris med 25:e och 75:e percentilen men ser inte framåt. Den kan därför ladda batteriet på natten fast solen hade fyllt det gratis några timmar senare. En optimering över hela horisonten hade gett en bättre plan.
- **Batteriet antas vara tomt vid midnatt** och saknar förluster. Det är en förenkling som gör planen mindre realistisk.
- **En tråd per klient** är enkelt men skalar inte. Med många klienter hade jag valt en trådpool eller `poll()`.
- **Cachen ligger bara i minnet.** Efter en omstart finns ingen plan förrän första hämtningen är klar. Att spara senaste plan på disk hade löst det.
- **Förbrukningen är ett konstant medelvärde.** En förbrukningsprofil över dygnet hade påverkat planen mer än något av det ovan.

## 9. Arbetsprocess och lärdomar

Jag byggde systemet nedifrån och upp i små steg med en commit per del: först loggning och konfiguration, sedan kön, fetchern, parse, compute, cache, servern och sist klienten. Varje modul fick enhetstester innan nästa byggdes ovanpå, vilket gjorde att fel hittades där de uppstod. Profileringen gjordes som ett eget steg med mätning före och efter.

Som hjälpmedel använde jag gprof, Valgrind och ThreadSanitizer, samt AI-assistenten Claude Code som stöd i kodningen och dokumentationen. Gruppens projekt har jag bara använt som referens för upplägg; koden här är skriven för den här inlämningen, med cJSON och libcurl som enda tredjepartsbibliotek.

Det viktigaste jag tar med mig:

- Bestäm vem som äger varje resurs. De flesta svåra frågor i projektet, om minne, fildeskriptorer och trådar, blev enkla när ägandet var tydligt.
- Tänk på nedstängningen från början. Den är svårare än starten och går inte att lägga på i efterhand.
- Mät innan du optimerar.
- Håll det litet. Ett system som går att förklara från början till slut är lättare att få rätt än ett som gör mer.
