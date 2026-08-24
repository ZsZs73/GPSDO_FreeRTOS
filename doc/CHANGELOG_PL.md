# Lista zmian — GPSDO FreeRTOS

[English](CHANGELOG_EN.md) | **Polski** | [Español](CHANGELOG_ES.md)

📖 [Strona projektu](../README.md) · Powrót do [README](README_PL.md)

Wszystkie istotne zmiany w projekcie są udokumentowane poniżej.

Projekt: **J. M. Niewiński** — <https://github.com/jmnlabs/GPSDO_FreeRTOS>
Na podstawie **GPSDO v0.06c** autorstwa André Balsy
(<https://github.com/AndrewBCN/STM32-GPSDO>); port na FreeRTOS oraz
algorytmy sterowania autorstwa autora, Claude AI jako asystent programowania,
projekt PCB — Scrachi (forum EEVBlog).

Sufiks `-rtos` oznacza linię portu na FreeRTOS.

---

## [v1.05-rtos SJ] — niewydane

Build dla Dave'a (Solder_Junkie) z EEVblog: v1.05 plus backport nieblokującego
zapisu raportu (`doc/v105-usb-cdc-nonblocking.patch`), w konfiguracji jego
sprzętu (OLED SSD1306, bez LTIC / PICDIV / GPS-TIMING / INA219 — algorytmy
10–12 wykrojone z kompilacji).

### Naprawione
- **Nieblokujący zapis raportu wyciszał telemetrię 1 Hz na USB CDC.**
  Oryginalny guard pytał `availableForWrite()` raz i dropował **cały** raport,
  gdy zwracał mniej niż jego rozmiar. Na USB CDC to każdy raport: kolejka TX
  `USBSerial` to `USB_FS_MAX_PACKET_SIZE *
  CDC_TRANSMIT_QUEUE_BUFFER_PACKET_NUMBER` = 64 × 2 = **128 bajtów**
  (domyślne stm32duino 2.12.0), a raport human-readable ma 400+. Boot i CLI
  działały dalej — krótkie linie, inna ścieżka — czyli build, który miał
  wyleczyć „wchodzi USB, ekran się zamraża", oddałby płytkę, której ekran już
  nie zamarza, ale telemetria nigdy się nie pojawia. Zapis jest teraz
  porcjony po to, co port potrafi przyjąć, z budżetem 25 ms: host czytający
  wynosi cały raport w kilku iteracjach, host nieczytający dostaje drop ogona,
  a wyświetlacz żyje. `room == 0` znaczy „pełna, czekaj" — nigdy ślepy zapis
  reszty: na pełnej kolejce CDC `USBSerial::write()` kręci się, dopóki host
  jest połączony, czyli to właśnie ten freeze. Kolejka TX CDC powiększona
  też do 1 KB (`-DCDC_TRANSMIT_QUEUE_BUFFER_PACKET_NUMBER=16` w
  `build_opt.h`; makro ma `#ifndef` w bibliotece USBDevice, sprawdzone na
  rdzeniu 2.12.0) — zdrowy host ma ~2,5 s zapasu i nic nie jest dropowane.
  Plik patcha niesie już poprawioną wersję.

---

## [v1.05-rtos] — 2026-08-20

Algorytm 12 doprowadzony do działania. W v1.04 wyszedł z poprawną arytmetyką i
pięcioma osobnymi usterkami w maszynerii wokół niej, z których każda zasłaniała
następną. Pętla trzyma teraz fazę na poziomie 5–8 ns RMS przez 23 godziny przy
jednym przezbrojeniu picDIV, wobec 10–23 ns najlepszego dotychczasowego
odniesienia — a każda poprawka poniżej została przesymulowana przed wgraniem, bo
dwie zmiany w tym projekcie, które poszły na samym rozumowaniu, obie okazały się
błędne.

### Naprawione
- **Estymator szumu mógł już tylko maleć.** Bramka odstających brzmiała
  `dp_lim = 5*sigma` i czytała estymatę, którą sama karmiła: gdy sigma raz
  zmalała, każda różnica dość duża, by ją podnieść, była odrzucana jako
  odstająca. Zmierzone 14.08 — `sig` czytało dokładnie 2 ns przez wszystkie 1020
  próbek przebiegu, a wyprowadzone z niej granice poziomów przypięły hierarchię
  do podłogi 100 jednostek: 79 z 80 korekcji odpaliło na poziomie 0. Akumulator
  wielopoziomowy, który nigdy nie opuszcza poziomu zerowego, nie jest
  wielopoziomowy. Bramka jest teraz bezwzględna (300 ns), a prawdziwe odstające,
  dla których powstała — różnice liczone w poprzek przerwy NOPH/SYNC/re-arm —
  są odcinane strukturalnie flagą ciągłości, a nie statystycznie. Sigma ma
  podłogę 5 ns, poniżej której ten detektor nie rozdziela uczciwie.
- **Człon częstotliwości miał odwrócony znak.** Nosił `+polarity`, przepisane z
  gałęzi częstotliwości algorytmu 11 — ale tamta gałąź czyta TIM2, a własny
  komentarz algo 11 w tym firmware zapisuje ustalenie sprzętowe, że TIM2 i
  detektor LTIC mają na tym okablowaniu przeciwną orientację. Nachylenie `f_nss`
  nie jest odczytem TIM2: to pochodna tych samych wartości akumulatora, które
  dają człon fazowy, z tego samego czujnika. Wielkość i jej własna pochodna po
  czasie, mierzone jednym czujnikiem, nie mogą wymagać przeciwnych znaków
  sprzężenia. `cvPWM` Alana zgadza się z tym — przepuszcza fazę i nachylenie
  przez jedną konwersję i dodaje je. Przy plancie zmierzonym, a nie założonym
  (+319,5 µHz/LSB, z regresji stusekundowej średniej PWM wobec drukowanej
  stusekundowej średniej częstotliwości, korelacja 0,999 przy zerowym
  opóźnieniu), stary znak dawał `d(phase_rate) = +0,4·f_ns`. To dodatnie
  sprzężenie.
- **`s_mla_wait` nigdy nie było zerowane.** Występowało w pliku dokładnie dwa
  razy — w deklaracji i przy `++` w teście rezygnacji — i nigdy nie wracało do
  zera. Około pięciu minut po starcie przekraczało 300 i test rezygnacji kasował
  flagę w tej samej sekundzie, w której była podnoszona, co zabijało obie rzeczy,
  które ta flaga bramkuje: korekcję przejścia przez zero i blokadę nowych
  korekcji w trakcie dochodzenia fazy. Przebieg, który to wykrył: `zc` = 7 przez
  76 minut, wszystkie w pierwszych pięciu, i 1072 z 1174 korekcji dokładnie dwie
  sekundy od siebie, czyli goła kadencja poziomu 0, gdy nic jej nie hamuje.
  Mechanizm przejścia przez zero nigdy więc nie działał dłużej niż pierwsze
  minuty jakiegokolwiek przebiegu od czasu wprowadzenia.
- **Ratunkowy FLL był regulatorem bang-bang i nie mógł być niczym innym.** Krok
  wynosił `-f*lsb_per_hz*0,10` klamrowany do ±64, co nasyca się przy
  |f| = 0,256 Hz, podczas gdy bramka poniżej otwierała się dopiero przy 0,3 Hz —
  człon proporcjonalny nie mógł więc zadziałać nigdy. Napędzany co sekundę ze
  stusekundowej średniej, czyli około 50 s opóźnienia, daje 64 LSB/s × 50 s =
  3200 LSB drogi, zanim pomiar zareaguje: 1,0 Hz przeregulowania. Obie liczby są
  w logach (462 z 655 kolejnych kroków dokładnie ±64; PWM przebiegające 12 845
  LSB; `f100` wahające się od −1,29 do +1,55 Hz). Teraz aplikuje całą policzoną
  korekcję raz i przytrzymuje przez tyle, ile potrzebuje na odświeżenie średnia,
  z której ją policzono, w dwóch biegach: średnia 10-sekundowa przy dużym
  błędzie, 100-sekundowa gdy zmaleje, a przytrzymanie zawsze odpowiada oknu w
  użyciu. Bramka zeszła z 0,3 Hz na 0,05 Hz, bo powyżej 0,147 Hz faza przebiega
  całe pasmo detektora ±940 ns w jednym horyzoncie 64 s — stara bramka
  zostawiała martwą strefę od 0,147 do 0,3 Hz, w której pętla fazowa nie
  dostawała dość długiego okna, a FLL uznawał swoją pracę za skończoną.
- **`instant_offset` zawijało się.** `FREQ_LOWER`/`FREQ_UPPER` dopuszczają
  ±500 Hz, a pole było `int8_t`, więc wszystko powyżej ±127 podawało śmieci
  każdej bramce, która je czytała. Teraz `int16_t`, we wszystkich trzech plikach,
  które go dotykają — w strukturze, w rzutowaniu, które je wypełnia, i w
  migawce, która je kopiuje. Naprawienie tylko jednego skompilowałoby się
  czysto i zostawiło zawijanie na miejscu.
- **Gałąź częstotliwości i gałąź FLL brały znak z zaszytego minusa.** Poprawne
  wyłącznie dlatego, że `LPOL` na tej płytce wynosi −1; na płytce z `LPOL +1` to
  dodatnie sprzężenie. Obie biorą teraz `polarity` z płytki, co tutaj wylicza się
  identycznie, a gdzie indziej poprawnie.
- **Dither zapisywał tylko jedną ze swoich dwóch tablic DMA.** Podwójne
  buforowanie zmienia tablicę co przebieg, więc druga — wciąż z poprzednim kodem
  — grała aż do następnego zapisu, a wyjście przeskakiwało między starą a nową
  wartością z częstością około 3 Hz (jeden przebieg to 2^(24−N) okresów nośnej =
  167,8 ms niezależnie od wyboru N). Filtr dwubiegunowy 0,8 Hz tłumi 3 Hz
  zaledwie 14×. Obie tablice są teraz wypełniane, pod muteksem, bo
  `pwm24_write()` jest osiągalne i z ControlTask, i z CliTask, a dwa równoległe
  wypełnienia jednej tablicy przeplatają się w rozdarty 168-milisekundowy
  przebieg. Nadpisywanie tablicy czytanej przez DMA jest bezpieczne przez
  wyprzedzenie: wypełnianie zapisuje wpis co kilka mikrosekund, gdy DMA
  konsumuje jeden co 81,9 µs.
- **`PO` i `AO` nie dawały się ustawić na zero.** Kontrola zakresu brzmiała
  `v >= −3000 && v <= 3000 && v != 0.0f`, więc jedyna wartość, której użytkownik
  najpewniej chce, była jedyną odrzucaną. Zakresy poprawione na ±5000 Pa i
  ±3000 m, a obie komendy podają teraz jednostki w pomocy i w echu.

- **Płytka nie zawsze wstawała po zimnym starcie, a szyna 3,3 V nigdy nie była
  przyczyną.** `ubx_poll_svin_nav()` wołało `vTaskDelay()` bezwarunkowo. Jego
  bliźniak `ubx_poll_svin()` ma strażnika i komentarz, który to wyjaśnia —
  *„before vTaskStartScheduler() this must not be vTaskDelay(): calling it with
  no scheduler hangs the system"* — a poprawka trafiła do jednej z tych dwóch
  funkcji i nie trafiła do drugiej.

  Przed startem schedulera `vTaskDelay()` pisze przez `pxCurrentTCB`, który jest
  jeszcze `NULL`, więc płytka wpada w hard fault, a domyślny handler kręci się w
  pętli z wyłączonymi przerwaniami: żadnego wyjścia, żadnego watchdoga, tylko
  przycisk reset. Haki awaryjne FreeRTOS dodane w v1.04 tego nie złapią —
  potrzebują działającego jądra.

  Usterka chowała się za kolejnością wywołań w `gpsdo_gps_init()`: NAV-SVIN jest
  odpytywane wyłącznie wtedy, gdy TIM-SVIN nie odpowie w swoim oknie 500 ms.
  Odbiornik, który już pracuje, odpowiada i płytka startuje — tak jest po
  resecie, bo odbiornik ma własne zasilanie. Odbiornik dopiero wstający nie
  odpowiada i płytka staje. To właśnie przypadek zimnego startu, i ta asymetria
  jest powodem, dla którego rzecz przez długi czas wyglądała na zasilanie
  siadające przy narastaniu szyny.

  Znalezione w logu z czterema kolejnymi startami, z których każdy kończył się
  po `UBX: CFG-NAV5 ACK`, a przed `LEA-T: starting survey-in`, przy przyczynie
  resetu `PIN/NRST` za każdym razem — czyli przycisku operatora. Dekoder wypisuje
  `POWER-ON/BROWN-OUT` i osobną linię o sprawdzeniu szyny 3V3, gdy winne jest
  zasilanie, i nie pojawiła się ani razu. Usterka zasilania nie zatrzymuje się
  cztery razy na tej samej linii kodu.
- **Cyfry częstotliwości nie podążały za stanem lock algorytmu 12.** Logika
  koloru ma gałąź autorytatywną dla pętli publikujących stan na żywo, a
  algorytmu 12 nie było na tej liście — spadał więc do gałęzi oceniającej lock ze
  średnich częstotliwości, czyli robił dokładnie to, przed czym ostrzega
  komentarz nad tą gałęzią.

  Zmierzone na przebiegu 2,99 h: pętla i kolor rozjeżdżały się przez **15,0%**
  próbek, a każdy taki przypadek to pętla w LOCK i białe cyfry — nigdy odwrotnie.
  Pętla weszła w LOCK w 108 s, cyfry zzieleniały w 1041 s. Piętnaście minut
  zdyscyplinowanego oscylatora wyglądającego na niezdyscyplinowany, przy każdym
  starcie, bo dopóki nie napełni się średnia 1000 s, ta gałąź nie ma czym
  oceniać i `locked` jest fałszywe z konstrukcji.

  Algorytm 12 bierze teraz kolor z własnego trendu, jak 10 i 11. `CORR` i `ZC`
  liczą się jako lock: to stany jednosekundowe znaczące, że pętla robi swoje —
  ta sama logika, dla której nie zerują `s_mla_quiet`. Bez tego cyfry mrugałyby
  na biało przy każdej korekcji, czyli szesnaście razy w zmierzonych trzech
  godzinach. Zgodność wynosi teraz 100%.
- **Bramka „stale echo" była ustawiona na pół kwantu.** Odbiera ona lock oparty
  na długiej średniej, gdy średnia 10-sekundowa odjechała, a próg wynosił
  ±50 mHz. Ale średnia 10 s to zliczenie cykli przez dziesięć sekund, więc jej
  ziarno to 0,1 Hz: przez trzy godziny przyjęła dokładnie trzy wartości —
  −100, 0 i +100 mHz — i nic pomiędzy. Próg 50 mHz nie znaczył więc „w granicach
  50 mHz", tylko „licznik musi pokazać dokładnie 10 000 000", a jeden kwant w
  którąkolwiek stronę gasił zieleń. To 8,3% ustalonych próbek i 46% opisanego
  wyżej rozjazdu. Teraz ±0,15 Hz: jeden pełny kwant plus pół kwantu zapasu, więc
  pojedyncze drgnięcie przechodzi, a rzeczywista utrata dyscypliny — wiele
  kwantów, czyli to, po co ta bramka istnieje — nadal ją zatrzymuje. Algorytmy
  0–9 mają tę samą bramkę i tę samą poprawkę.

### Dodane
- **Bramka poziomowa na człon częstotliwości.** Własny szum nachylenia to
  `sd(f_nss) = sigma · 2^((1−3L)/2)`, więc na poziomie 0 jest to 1,41·sigma
  czystego szumu skalowanego przez 12,5 LSB na ns/s, wobec 0,39 LSB na ns dla
  członu fazowego — przewaga 32:1 na rzecz niewłaściwej wielkości. Log pokazał,
  co to kupuje: 46% korekcji uderzało w klamrę ±470, jedna z nich przy fazie
  czytającej dokładnie 0 ns i korekcji na pełnej skali. Człon jest teraz używany
  od poziomu 3 w górę, gdzie ta sama estymata jest uśredniona po parach
  16-sekundowych i znów jest pomiarem.
- **Trim częstotliwości z TIM2.** Gdy średnia stusekundowa pokazuje więcej niż
  0,03 Hz, składowa częstotliwościowa korekcji brana jest z tego pomiaru zamiast
  z nachylenia akumulatora. W stanie ustalonym jest uśpiony — w dziesięciogodzinnej
  symulacji nie odpalił ani razu — i o to właśnie chodzi: łapie wyskoki
  częstotliwości, które inaczej wyprowadziłyby fazę poza pasmo detektora, więc
  pętla nigdy nie musi wchodzić w lock od nowa. Dwudziestotrzygodzinny przebieg,
  który ustalił powyższe liczby, zanotował **jedno** przezbrojenie picDIV, wobec
  121 przy tych samych nastawach bez niego.
- **`configUSE_MUTEXES` i `INCLUDE_xTaskGetSchedulerState`**, ustawione jawnie.
  Blokada dithera potrzebuje obu, żadne nie było ustawione przez ten projekt, a
  tego, czy domyślna konfiguracja biblioteki je włącza, nie zostawia się
  przypadkowi: brakujące makro to błąd kompilacji, nie niespodzianka w runtime.

- **Dolne 8 bitów ditheru dociera wreszcie do pętli.** v1.04 wypuściła wyjście
  24-bitowe i napisała w tym changelogu, że pętla nie dostaje jeszcze
  drobniejszego kroku: każdy algorytm wołał `gpsdo_dac_write16()`, które
  przesuwało wartość w górne 16 bitów, żeby zapisane ustawienia zachowały swoje
  napięcie, a dolny bajt zawsze był zerem. Już nie jest.

  Ułamek należy do `gpsdo_dac.cpp`, nie do pętli sterowania, i to jest cały
  pomysł. Wartość sterująca zapisywana jest z 21 miejsc — z przemiatań `CT` i
  `LC`, z ramp akwizycji, ze sterowania w holdoverze, z `SP` i z samej pętli — a
  dwadzieścia z nich jest zgrubnych z rozmysłem: przemiatanie, które zatrzyma się
  na 30720,4 zamiast na 30720, nie jest lepszym przemiataniem, tylko takim,
  którego punktu odniesienia nikt nie potrafi podać. Każdy zgrubny zapis kasuje
  ułamek przy okazji tego, że trafia do `gpsdo_dac_write16()`, więc żaden
  wywołujący nie musi o tym pamiętać. Trzymanie ułamka w pętli oznaczałoby
  dwadzieścia miejsc, z których każde musiałoby wiedzieć, że ma go wyzerować — a
  to dokładnie ta klasa błędu, dla której ten jeden punkt zapisu powstał.

  Co to daje na zmierzonym tutaj obiekcie: jeden krok 16-bitowy to około 320 µHz,
  czyli 3,2e-11 z 10 MHz — grubiej, niż 4e-12, które pętla zmierzono trzymała
  przez 10 000 s. Dochodziła tam ditherując między sąsiednimi kodami z korekcji
  na korekcję, co działa, ale zostawia napięcie sterujące w ciągłym polowaniu. Z
  zachowanym ułamkiem korekcja mniejsza od jednego kroku jest stosowana zamiast
  obcinana, a krok schodzi do 1,25e-13.

  Obcięcie, które znika, było przy tym stronnicze: `(int32_t)` zaokrągla w stronę
  zera, więc każda korekcja traciła część siebie w tę samą stronę — co pętla
  widzi jako błąd wzmocnienia sięgający jednej szóstej przy korekcjach rzędu
  6 LSB, obserwowanych w normalnej pracy.

  Powyżej warstwy DAC nie zmieniło się nic. `gpsdo_dac_last16()` nadal zwraca
  zwykłe `uint16_t`, więc wyświetlacze, linia telemetrii i flash ring widzą
  dokładnie to, co widziały wcześniej, a blok ustawień nadal zapisuje 16 bitów:
  odtworzenie startuje z zerowym ułamkiem i oddaje co najwyżej 1,25e-13, czyli
  mniej, niż ten sprzęt jest w stanie pokazać.
- **`DAC` — komenda, która mówi, czym naprawdę jest napięcie sterujące.** Ścieżka
  wyjściowa, a dla ditheru częstotliwość nośnej i RAM zajęty przez tablice; kod w
  trzech ujęciach — 24-bitowym, zaokrąglonym 16-bitowym, którego używają
  wyświetlacze i flash ring, oraz dokładnym ułamkowym wraz z różnicą względem
  zaokrąglonego; zmierzone Vctl; oraz wielkość kroku dla obu szerokości, w µHz i
  jako ułamek 10 MHz. Kod 24-bitowy niebędący wielokrotnością 256 jest dowodem na
  to, że pinem steruje ścieżka precyzyjna — i dlatego drukowane są wszystkie trzy
  ujęcia, a nie jedno; komenda mówi też wprost, czy ścieżka precyzyjna jest
  aktywna, czy wyjście i tak ją zaokrągla.

  Liczby kroku wymagają wzmocnienia obiektu, którego dostarcza tylko `CT`. Bez
  niego komenda to mówi, zamiast drukować liczbę wyprowadzoną z wartości
  domyślnej. Wpisana także do zakładki Help w tunerze.

- **`MF` i `MFT` — limity per poziom dostają własne źródło, wybierane niezależnie
  od wzmocnienia.** Oba siedziały w jednym `if`, więc `MG 0` znaczyło
  „wzmocnienie z CT **i** limity z formuły szumu", a `MG > 0` — „wzmocnienie
  ręcznie **i** limity ręcznie". Nie ma powodu, by były zespawane: wzmocnienie
  należy do OSCYLATORA (jest w LSB na ns, a inny OCXO ma inną czułość Vctl),
  natomiast limity należą do SZUMU FAZY, jaki widzi płytka, czyli do miejsca i
  odbiornika. „Zmierzone wzmocnienie, ręczne limity" — dokładnie to, czego
  potrzebuje hałaśliwa instalacja — nie dało się w ogóle wyrazić.

  `MF 0` śledzi `MG` jak dotąd i jest domyślne, więc bez wyraźnej prośby nic się
  nie zmienia. `MF 1` trzyma tablicę zapisaną, `MF 2` formułę szumu, `MF 3`
  tablicę mierzoną poniżej. Oba ustawienia mieszczą się w trzech bajtach
  wyrównania, które blok algo-12 już miał, więc układ, rozmiar i `SETTINGS_VER`
  zostają bez zmian, a starszy zapis nadal się ładuje — odczytuje się jako 0/0,
  czyli dokładnie zachowanie tamtej wersji.
- **`MF 3` — limity per poziom mierzone zamiast ekstrapolowanych.** Formuła
  brzmi `thr[L] = 8·σ·√(2^L)·√10`, a `√(2^L)` mówi, że faza jest BIAŁA, czyli że
  uśrednienie 2^L próbek zbija test jak 2^(L/2). Zmierzone na dwóch płytkach tej
  konstrukcji — to samo PCB, ten sam OCXO, różne pomieszczenia — wykładnik
  wynosi **0,95 i 1,03**, a nie 0,50. Uśrednianie prawie nic tu nie daje, bo
  liczy się wolna wędrówka (autokorelacja 0,96 przy 60 s, 0,64 przy 300 s), a nie
  szum próbka-do-próbki. Błąd rośnie z poziomem: formuła zaniża rzeczywisty
  rozrzut około 5× na poziomie 0 i ponad 100× na poziomie 10, więc jej tablica
  opada 32× w skali hierarchii tam, gdzie sama faza opada 1,3×.

  Wykładnik jest więc mierzony. Każdy poziom trzyma średni kwadrat swojej własnej
  statystyki testowej, dopasowanie najmniejszych kwadratów log2(sd) względem
  poziomu daje amplitudę i wykładnik naraz, a tablica powstaje z dopasowania.
  Dopasowanie PO poziomach, a nie zaufanie każdemu z osobna, jest tym, co czyni
  je użytecznym wcześnie — poziom 8 jest testowany raz na 512 s i sam
  potrzebowałby pół doby na własną wariancję, ale niskie poziomy zapełniają się w
  minuty i dopasowanie ekstrapoluje.

  Znika przy tym pięć liczb wpisanych na sztywno: wykładnik 0,5, mnożnik `8.0`
  (teraz kwantyl rozkładu normalnego dla częstości fałszywych strzałów zadanej
  przez `MFT` — czyli robota, którą ósemka wykonywała ręcznie, bo hierarchia
  testuje poziom 0 tysiąc razy częściej niż poziom 10), propagacja białego szumu
  `√10`, podłoga σ na 5 ns — właściwość tego detektora, nie arytmetyki — oraz
  podłoga 100 jednostek. Zostaje jedna liczba o fizycznym znaczeniu: co ile
  czasu wolno wystąpić korekcji wywołanej samym szumem.

  Wykładnik jest klamrowany do [0,5; 1,0] i jest to fizyka, nie gust. Poniżej 0,5
  uśrednianie usuwałoby więcej, niż pozwala biały szum; powyżej 1,0 rozrzut
  rośnie szybciej niż płasko-w-ns, czyli mamy RAMPĘ fazy, a nie hałaśliwszą
  płytkę — a wpuszczenie rampy do progu to awaria już w tym pliku zapisana, gdzie
  sigma wspięła się 165 → 746 ns i pętla zamarzła.

  Zweryfikowane przez odtworzenie arytmetyki firmware po zapisach obu płytek:
  tablica warsztatowa wychodzi 74 ns opadające do 14, czyli tam, gdzie ta płytka
  została ustawiona ręcznie po tym, jak auto okazało się niestabilne, a płytka
  domowa odtwarza własne ustalone zachowanie. Na trzygodzinnym przebiegu dom
  dopasował **α = 1,00** i korygował na poziomach od 5 do 9 — po raz pierwszy ta
  hierarchia użyła więcej niż jednego czy dwóch swoich poziomów.

  **Nie jest to automatycznie lepsze.** Na płytce domowej, gdzie znacznie
  ciaśniejsza tablica z formuły przypadkiem pasowała do cichego miejsca, tablica
  mierzona podwaja RMS fazy (mediana 11,6 ns wobec 5,5 ns w oknie tej samej
  długości), bo koryguje trzy razy rzadziej. Te dwie tablice zadają różne
  pytania — formuła pyta, czy odchylenie przekracza szum pomiarowy, a tablica
  mierzona, czy jest nietypowe dla tej płytki — i to, która ma rację, zależy od
  miejsca. Po to właśnie jest `MF`.

### Zmienione
- `LOCK` w polu trendu znaczy teraz, że hierarchia jest cicha **oraz** że
  częstotliwość z TIM2 mieści się w 0,05 Hz, liczone po kolejnych cichych
  sekundach, a nie po `s_mla_count`, który zeruje się przy każdej korekcji i był
  kiepskim wskaźnikiem tego, jak dawno cokolwiek się wydarzyło.

- **`GPSDO_PWM_DITHER` jest włączony w wysyłanej konfiguracji.** W v1.04 wyszedł
  wyłączony, dopóki ścieżka wyjściowa nie była sprawdzona; po domknięciu ścieżki
  precyzyjnej i 23-godzinnym przebiegu „wyłączone" przestało być uczciwym
  domyślnym ustawieniem. Zakomentowanie go nadal wraca do zwykłego PWM
  16-bitowego, a pin, filtr i okablowanie są w obu przypadkach te same.
- **Układ pól na panelu 320×240 jest wreszcie taki jak na 480×320.** Ten podręcznik
  od v0.93 pisze, że ekran roboczy jest projektowany raz i skalowany — i to była
  prawda o geometrii, a nie o treści: oba panele rozjechały się pole po polu.
  qErr przeniósł się do wiersza Alt, obok danych fiksu, do których należy; AHT i
  pole fazy zamieniły się kolumnami, więc czujniki środowiskowe dzielą lewą
  kolumnę, a elektryczne prawą; Vcc i Vdd zajęły wspólnie zwolniony wiersz. Mały
  panel pokazuje wszystko to, co duży.

  Każde pole, które łączyło etykietę z wartością o zmiennej szerokości, zostało
  rozbite na dwa. Pojedynczy napis kotwiczony do prawej unieruchamia jednostkę, a
  etykietę ciągnie na boki wraz ze zmianą szerokości cyfr — na qErr widać to było
  jako etykietę skaczącą raz na sekundę. Etykieta i wartość to teraz osobne sloty
  z osobnym paddingiem: etykieta trzyma lewą krawędź kolumny, wartość zachowuje
  prawą kotwicę, a zmienia się tylko odstęp między nimi. Tak samo `dph` i prąd
  INA.
- **Font 2 jest proporcjonalny, a ten układ był liczony po 8 px na znak.**
  Sprawdzone z tablicą szerokości samej biblioteki: to zawyża napisy małego panelu
  o jakąś jedną piątą — `Vph:1.951V` mierzy 70 px, nie 80. Błąd nie był
  akademicki: to on kosztował etykietę `dph` i to on trzymał Vcc na dwóch
  miejscach po przecinku tam, gdzie 480 pokazuje trzy. Obie rzeczy wróciły. Pola
  po prawej dzielą teraz jedną linię wyrównania na x=314 — tę, na której Vdd
  siedziało od dawna — więc qErr, dph, prąd INA i Vdd tworzą kolumnę zamiast
  czterech prawie-trafień. Padding każdego pola to teraz zmierzona szerokość jego
  własnej najszerszej formy, a nie dzisiejszego odczytu, i paddingi w wierszu
  kafelkują go dokładnie, więc żadne tło nie może zjeść krawędzi sąsiada.

### Podziękowania
- **Alan Cashin** (MIS42N z forum EEVBlog) jest teraz wymieniony tam, gdzie praca
  jest jego: w `V`, w nagłówku pomocy, na ekranie About tunera i w tabeli
  podziękowań wszystkich trzech instrukcji. Algorytm 12, korekcja przejścia przez
  zero, ditherowany PWM i pomysł na samoocenę `CS` pochodzą z jego Budget GPSDO.
  Dotąd figurował jako „dither / DAC discussion", co znacznie to zaniżało.

### Zmierzone
Dwadzieścia trzy godziny, progi automatyczne, `MR 9`, dither 13-bitowy:

| | ten przebieg | najlepszy poprzedni |
|---|---|---|
| faza RMS, po osiadaniu | **5–8 ns** | 10–23 ns |
| \|faza\| < 10 ns | **86,7%** próbek | — |
| przezbrojenia picDIV | **1** | 121 |
| osiągane poziomy korekcji | **typowo 5–6, do 8** | 0 |
| częstotliwość na 10 000 s | **4e-12** | 1,4e-11 |
| odstęp między korekcjami | 254 s | 130 s |

`NOPH` trzy razy na 82 572 próbki; `FLL` raz. Ciśnienie otoczenia spadło w trakcie
przebiegu o 4 hPa, a pętla nie zareagowała.

---

## [v1.04-rtos] — 2026-08-12

### Dodane
- **`GPSDO_PWM_DITHER` — 24-bitowe napięcie sterujące z krótkiego PWM z ditherem.**
  Pomysł Alana Cashina (MIS42N): puść PWM na mniejszej liczbie bitów, niż
  potrzebujesz, i zmieniaj wypełnienie z okresu na okres tak, by resztę niosła
  średnia.

  Zyskiem jest NOŚNA, a nie dodatkowe bity. Tętnienie trzeba odfiltrować poniżej
  jednego kroku wyjścia, a jak trudne to jest, zależy od odstępu między nośną a
  zakresem filtru: PWM 16-bitowy przy 2 kHz pozwala na zakres 0,7 Hz i stałą
  czasową 230 ms, a dither 13-bitowy przy 12,2 kHz — na 4,2 Hz i 38 ms.
  Opóźnienie filtru wchodzi do pętli wprost jako przesunięcie fazy, więc
  sześciokrotnie krótszy filtr jest wart więcej niż sama rozdzielczość.

  Alan ditheruje w przerwaniu timera, bo PIC nie ma DMA. Tutaj byłoby to 12 000
  przerwań na sekundę konkurujących z przechwytem 1PPS — jedynym przerwaniem,
  którego nie wolno opóźniać. Ale wzór dla stałej wartości jest okresowy, więc
  jest liczony raz do tablicy i odtwarzany przez DMA do rejestru porównania:
  0,012% CPU przy 13 bitach i ani trochę tego w przerwaniu. Średnia jest dokładna
  z konstrukcji — tablica trzyma dokładnie Y wpisów o wartości X+1 wśród
  2^(24-N).

  Ten sam pin co dotąd (PB9, TIM4 CH4), więc dotychczasowy filtr i okablowanie
  zostają bez zmian. TIM4_UP steruje DMA1 Stream 6 Channel 2; takt 2 Hz jest na
  TIM9, a łańcuch 1PPS na TIM2/TIM3, więc nic innego nie jest ruszane. Dwa bufory
  w sprzętowym trybie double-buffer sprawiają, że zmiana wartości nigdy nie daje
  glitcha na pinie.

  Domyślnie wyłączone. Kosztuje 8 KB RAM przy 13 bitach, 16 KB przy 12.

  **Czego to jeszcze nie daje**, to drobniejszego kroku dla pętli: każdy algorytm
  woła `gpsdo_dac_write16()`, które przesuwa wartość w górne 16 bitów, żeby
  zapisane ustawienia zachowały swoje napięcie. Dolne 8 bitów czekają na pętlę,
  która zawoła `gpsdo_dac_write24()`.
- **Korekcja przy przejściu przez zero, ze schematu Alana.** Po korekcji
  granicznej, która zmienia częstotliwość, faza idzie dalej w tę stronę, w którą
  już szła: przemiata przez zero, wychodzi po drugiej stronie i zwykle znów
  przekracza granicę — więc pętla koryguje, przestrzeliwuje, koryguje z powrotem i
  osiada powoli.

  Chwila przejścia fazy przez zero jest szczególna. Błąd fazy jest zerowy, ale
  błąd częstotliwości, który ją tam doprowadził, wciąż istnieje; skasowanie błędu
  częstotliwości dokładnie wtedy zostawia oscylator z właściwą częstotliwością ORAZ
  bez błędu fazy — zamiast w stanie, do którego pętla musi dopiero dochodzić
  kolejnymi iteracjami.

  Zmierzone względem własnych logów Alana z tej samej konstrukcji: jego pętla
  koryguje co 506 sekund, gdzie ta korygowała co 130. Większość tej różnicy to
  właśnie ten test, który Alan nazywa niezbędnym, a którego tutaj brakowało.

  Raportowane jako `zc=` w telemetrii; trend pokazuje `ZC` w momencie zadziałania.
- **Algorytm 12 — akumulator wielopoziomowy.** Wg konstrukcji Alana Cashina
  (MIS42N z forum EEVblog). Każda inna pętla tutaj ma jedną stałą czasową, a ta
  jest kompromisem, którego nikt nie wygrywa: zmierzone względem wzorca
  rubidowego, `LTC 60` jest do 1,58× lepsze powyżej 800 s, a `LTC 240` do 1,44×
  lepsze między 10 a 400 s. Ta pętla nie wybiera. Odczyty gromadzą się w
  poziomach — poziom n obejmuje 2^n sekund — a korekcja następuje na
  **najniższym** poziomie, którego błąd przekracza granicę. Duży błąd działa w
  ciągu dwóch sekund, mały czeka na dłuższe uśrednienie. Nie ma `LTC` do
  ustawienia.

  Poziomy wynikają z układu bitów licznika sekund, a nie z tablicy buforów:
  jedenaście poziomów, od 2 s do 2048 s, za 22 bajty.

  **Wejściem jest faza w nanosekundach z detektora LTIC.** Pierwsza wersja
  karmiła algorytm błędem zliczeń TIM2 w całych hercach i była ślepa —
  zdyscyplinowany oscylator siedzi daleko poniżej 1 Hz, więc pole czytało zero w
  83% i 95% próbek w dwóch przebiegach, a akumulator nie gromadził nic. Faza się
  całkuje tam, gdzie sekundowy pomiar częstotliwości nie. Alan zapytał, dlaczego
  podałem 100 ns, skoro TIC rozdziela 1 ns — miał rację, podałem rozdzielczość
  licznika zamiast detektora.

  **Test częstotliwości został usunięty**, zgodnie z radą Alana: *„To był
  eksperyment... chcemy stabilnego układu, w którym testy zawsze przechodzą. Więc
  test częstotliwości jest zbędny."*

  Nowe komendy `MG`, `MR`, `MLP` i `ML`, zapisywane przez `ES ALGO12`. Granice
  poziomów są edytowalne i utrwalane, bo tylko **jedna** została kiedykolwiek
  wyprowadzona — 125 ns przy 128 s, ze specyfikacji 10 MHz ±0,01 Hz. Resztę Alan
  nazywa arbitralną.
- **Raportowanie przyczyny resetu przy starcie.** `RCC->CSR` jest odczytywany i
  dekodowany, zanim cokolwiek innego ruszy, więc sporadyczny restart nie wygląda
  już identycznie niezależnie od tego, czy przyszedł z zaniku zasilania, pinu
  reset czy resetu programowego. Dodane po tym, jak płytka restartowała się
  wielokrotnie w tym samym miejscu konfiguracji GPS, bez możliwości rozstrzygnięcia
  przyczyny.
- **Haki awaryjne FreeRTOS i własny `STM32FreeRTOSConfig.h`.**
  `configCHECK_FOR_STACK_OVERFLOW` i `configUSE_MALLOC_FAILED_HOOK` domyślnie są
  zerami, więc przepełniony stos po cichu psuje sąsiada, a `configASSERT` wpada w
  `for(;;)` z wyłączonymi przerwaniami — martwy biały panel i cisza na konsoli.
  Dokładnie tak wyglądały trzy ostatnie awarie: za mały stos CLI przy zapisie do
  flash ringu, odczyt pustej grupy zdarzeń przed startem schedulera i struktura w
  martwej gałęzi, która i tak powiększyła ramkę zadania wyświetlacza.

  Nadpisanie włącza oba haki i przedefiniowuje `configASSERT`, by wypisał plik i
  numer linii przed zatrzymaniem. Haki nazywają winne zadanie na konsoli USB i
  migają diodą, więc następna awaria przedstawi się sama w kilka sekund. Goły
  `Serial`, nie `OUT_SERIAL`: hak nie może dotykać muteksu ani strumienia
  Bluetooth, który sam może być przyczyną awarii.

  Autorstwo pliku: GLM-5.2, przyjęte tu zasadniczo bez zmian.

### Naprawione
- **Progi algorytmu 12 są teraz mierzone, a nie odziedziczone.** Były wzięte z
  konstrukcji Alana i przeskalowane stosunkiem kroków licznika, co jest złą
  wielkością: próg musi przekraczać **szum** pomiaru fazy, a ten różni się między
  konstrukcjami z powodów, których rozmiar kroku nie oddaje. Zmierzone na tej
  płytce: średnia fazy −1 ns przy odchyleniu standardowym 462 ns — oscylator był
  poprawnie ustawiony, a całe to rozrzucenie to szum, podczas gdy próg poziomu 0
  wynosił 462 ns. Przekraczało go 41% próbek. 620 korekcji w 1685 sekundach,
  hierarchia resetowana co 2,7 s i nigdy nieosiągająca poziomu 2.

  Firmware szacuje teraz szum fazy na bieżąco i wylicza z niego próg każdego
  poziomu. Przy okazji wyszedł drugi błąd: próg dotyczy wyrażenia testowego
  |3b − a|, którego odchylenie wynosi sigma·√(2^L)·√10, a nie średniej fazy, dla
  której jest to sigma/√N. Użycie drugiego czyniło próg 4,5× za niskim na poziomie
  0 i gorzej wyżej. Sześć sigma na właściwej wielkości daje odstęp między
  korekcjami rzędu minuty, wobec 256 s, na których osiada konstrukcja Alana.

  `ML` raportuje zmierzony szum i to, czy granice za nim podążają. W telemetrii
  jest jako `sig=`. Ustawienie `MG` powyżej zera zatrzymuje autotuning.
- **Algorytm 12 ignorował polaryzację detektora i mylił nanosekundy z hercami.**
  Dwie usterki w tym samym przeliczeniu, znalezione razem z jednego logu.

  `LPOL -1` nie było stosowane w ogóle — algorytm 11 mnoży swój człon fazowy przez
  `-polarity`, a ten nie — więc na takiej płytce każda korekcja szła w złą stronę.
  Do tego średnia faza w nanosekundach była mnożona przez LSB-na-herc, jakby to
  była ta sama wielkość: skasowanie P ns w czasie T sekund wymaga P/(100·T) Hz przy
  10 MHz, więc korekcja wychodziła 100·T razy za duża — od 200× na poziomie 0 do
  102 400× na poziomie 9. Każda uderzała w ogranicznik ±2000.

  Dodatnie sprzężenie zwrotne cztery rzędy wielkości za silne to uczciwy opis tego,
  co pokazał log: 6000 kroków wahnięcia PWM w 148 korekcjach.
- **`MG` i `MR` były przyjmowane i zapisywane, ale nigdy nieczytane.** Komendy
  działały, tuner je wysyłał, `ML` je odczytywało, a algorytm nie używał żadnej —
  ręcznie zadane wzmocnienie nic nie robiło, a wymuszony poziom korekcji nie
  istniał. Oba są teraz podłączone.
- **Algorytm 12 wymaga teraz detektora LTIC i wstrzymuje się zamiast zgadywać.**
  Był napisany tak, by na płytkach bez detektora całkować błąd zliczeń TIM2. Ta
  gałąź awaryjna była wręcz szkodliwa: zliczenia są skwantowane do całych herców i
  na zdyscyplinowanym oscylatorze czytają zero, więc ich całkowanie dawało
  błądzenie losowe szumu kwantyzacji, a nie fazę. Błądzenie przekraczało granicę
  poziomu, korekcja uderzała w ogranicznik, oscylator był wyrzucany na tyle, że
  detektor lądował na szynie, a szyna utrzymywała gałąź awaryjną przy życiu.
  Zmierzone: 6000 kroków wahnięcia PWM w 148 korekcjach, detektor na szynie przez
  58% czasu, a raportowana faza zablokowana na zerze.

  `LA 12` odmawia teraz bez `GPSDO_LTIC`, a gdy detektor jest, ale nie czyta,
  algorytm wstrzymuje się i pozwala pracować pomostowi picDIV. Cicha gałąź
  awaryjna niszcząca lock jest gorsza niż odmowa uruchomienia.
- **Algorytm 12 zbroi teraz picDIV.** Nie robił tego, a awaria była cicha: przy
  rampie na szynie detektor nigdy nie zwraca poprawnego odczytu, więc kod spadał
  do całkowania błędu zliczeń i algorytm znów był ślepy — dokładnie w sposób,
  któremu przejście na detektor miało zapobiec, bez żadnego śladu w telemetrii.
  Ten sam pomost z opóźnieniem, co w algorytmie 11.
- **Tuner przestał cokolwiek odczytywać z płytki.** `STATE_HINT` zostało dodane do
  `TelemetryParser`, ale użyte jako `self.STATE_HINT` z `GpsdoTuner` — innej klasy.
  Każda linia telemetrii rzucała wtedy `AttributeError` w obsłudze linii, więc
  żadna odpowiedź nie docierała do swojego absorbera i ani pola kalibracji z `LL`,
  ani tablica granic algorytmu 12 się nie wypełniały. Dwa objawy, jedna awaria.

  Obsługa jest teraz opakowana: błąd parsowania kosztuje jedną linię i wpis w
  monitorze, zamiast po cichu zabijać odbiór.
- **`MG` i `LG` odpowiadały tak samo — `gain=`.** Absorber Larsa biegnie pierwszy
  i przechwytywał odpowiedź algorytmu 12. Firmware odpowiada teraz `m_gain=` i
  `m_run_level=`.
- **Tabela granic w tunerze pokazywała zera.** Nigdy nie była odczytywana:
  zapytanie o parametry obejmowało tylko skalary, więc jedenaście pól stało na
  zerach, a naciśnięcie wysyłki nadpisałoby zerami tablicę, dla której firmware ma
  wartości domyślne. Tabela jest teraz czytana przy połączeniu, a wysyłka odmawia,
  dopóki którykolwiek wiersz jest zerem.
- **Płytka nie startowała: brak diody, brak konsoli, nic.** `setup()` zapisuje DAC
  trzy razy, zanim wykona się `xEventGroupCreate()`. Nowe statystyki korekcji wiszą
  na ścieżce zapisu DAC, a ich bramka czytała `xSysEvents`, wtedy jeszcze puste.
  Ramka stosu jest wymiarowana przy kompilacji, więc struktura w gałęzi, która
  nigdy się nie wykonuje, i tak rezerwuje miejsce przy każdym wywołaniu; dwadzieścia
  bajtów przelało stos zadania wyświetlacza i zginęło ono przed `tft.init()`.

### Zmienione
- **`SETTINGS_VER` 4 → 5** dla bloku algorytmu 12, **z migracją**. Blok v4 jest
  przyjmowany, jego pola stosowane, a wartości algorytmu 12 zostają domyślne.
  Odrzucenie go odebrałoby działający PID, LC i strefę czasową tylko dlatego, że
  doszedł nowy algorytm.

## [v1.03-rtos] — 2026-08-01

Zbudowane na v1.01. Eksperymenty z v1.02 — przetwornik delta-sigma na PB5 oraz
obsługa rdzenia STM32duino 3.0.0 — nie zostały przeniesione: pierwszy został
zmierzony i nie dawał tego, co obiecywał, drugi zawieszał płytkę na sprzęcie.
v1.01 pozostaje sprawdzoną bazą, z dwoma dodatkami.

### Naprawione
- **Ciepły restart nie uruchamia już od nowa ukończonego survey-in.** Odbiornik
  zachowuje przez `RB` własne zasilanie i własny stan, więc survey ukończony przed
  resetem jest nadal ważny: pozycja, którą ustalił, się nie zmieniła. Firmware
  wcześniej i tak zlecało nowy survey, odrzucając wynik, który powstawał minutami,
  i wypychając moduł z Time Mode na czas powtarzania wykonanej już pracy.
  `gpsdo_gps_init()` odpytuje teraz najpierw TIM-SVIN i pomija start, gdy
  odbiornik zgłasza valid=1 przy active=0 — czyli Time Mode z ukończonym survey za
  sobą. Zgłaszane jako *already in Time Mode from an earlier survey*.

  Wymagało to uczynienia `ubx_poll_svin()` bezpiecznym do wywołania przed
  schedulerem: funkcja oddawała procesor przez `vTaskDelay()` bezwarunkowo, co
  zawiesza system, gdy scheduler jeszcze nie działa. Teraz w takim przypadku używa
  `delay()` — tego samego wzorca, który stosował już czytnik ACK.
- **Płytka nie startowała: brak LED, brak konsoli, nic.** `setup()` zapisuje DAC
  trzy razy — początkowe 127, odczytany PWM i wartość domyślną — zanim wykona się
  `xEventGroupCreate()`. Nowe statystyki korekcji wiszą na ścieżce zapisu DAC, a
  ich bramka czytała `xSysEvents`, które w tym momencie było jeszcze NULL. Podanie
  NULL do `xEventGroupGetBits()` wyzwala `configASSERT` i zatrzymuje procesor,
  więc awaria następowała przed pierwszym mignięciem i nie zostawiała na konsoli
  żadnego śladu. Bramka sprawdza teraz najpierw NULL; te wczesne zapisy to
  komendy, nie korekcje, więc ich wykluczenie jest zarazem bezpieczne i poprawne.

### Dodane
- **`CS` — statystyki korekcji, czyli pętla oceniająca samą siebie.** Algorytm 11
  został zweryfikowany względem wzorca rubidowego na cudzym stanowisku; prawie
  nikt, kto to zbuduje, takiego nie ma, a bez niego zostaje słowo autora i
  wskaźnik locka. Korekcja, którą stosuje pętla, jest błędem, który przed chwilą
  zaobserwowała, więc wielkość tych korekcji mówi, czy dyscyplinowanie działa — a
  odniesieniem jest GPS, więc nie ma nic lepszego do porównania częstotliwości.
  Firmware i tak liczyło te wartości i je wyrzucało.

  Podaje RMS korekcji na oknach ostatnich **100, 1 000, 10 000 i 100 000
  korekcji** — w jednostkach DAC oraz, gdy `CT` zmierzyło nachylenie oscylatora,
  we względnej częstotliwości, wprost porównywalnej z liczbą z ADEV. Także stałe
  odchylenie, niezerowe wtedy, gdy pętla nadąża za rzeczywistym dryfem, a nie za
  szumem.

  Okna liczą korekcje, a nie sekundy, bo tempo korekcji zależy od algorytmu:
  algorytm 11 steruje raz na sekundę, algorytm 10 raz na `LIV`. Oznaczenie ich w
  minutach znaczyłoby co innego przy jednym algorytmie i sześćdziesiąt razy tyle
  przy drugim — ta sama liczba opisywałaby dwa różne przedziały. `CS` mierzy
  rzeczywisty odstęp i wypisuje, ile okna aktualnie obejmują w czasie
  rzeczywistym, żeby czytelnik nie musiał tego przeliczać. Przy jednej korekcji na
  sekundę 100 000 pokrywa około 28 godzin.

  To wagi wykładnicze, nie ostre okna: mniej więcej 63% wagi mieści się w N
  korekcjach, a 95% w 3N. Kosztuje to cztery mnożenia z dodawaniem na korekcję i
  zero pamięci, podczas gdy bufor na 100 000 próbek zająłby większość dostępnego
  RAM-u, odpowiadając na to samo pytanie nie lepiej.

  Liczone wyłącznie przy zalockowanej pętli i bez trwającej kalibracji: rampa
  akwizycji, trzy skoki `CT` i przemiatanie `LC` to komendy, nie korekcje, a jedna
  taka zdominowałaby średnią godzinną długo po tym, jak się skończyła. Algorytmy
  0-9 nie mają stanu locka, na którym dałoby się oprzeć bramkę, więc są wykluczone
  — i `CS` mówi to wprost, zamiast podawać liczbę bez określonego znaczenia.

  **Zastrzeżenie jest w wyjściu, w nagłówku i w README:** mierzy, czy PĘTLA JEST
  USTALONA, a nie czy WYJŚCIE JEST DOBRE. Zaszumiony detektor sprawia, że pętla
  goni szum; korekcje rosną, `CS` wiernie je raportuje, a oscylator był w
  porządku, dopóki pętla go nie popsuła. Nic mierzonego wewnątrz pętli tego nie
  zobaczy.

  Pomysł jest Alana (MIS42N z forum EEVblog), którego własna konstrukcja opiera
  się dokładnie na tym i dlatego nie potrzebuje wzorca wtórnego.
- **`GPSDO_DAC_EXT` — zewnętrzny przetwornik SPI, planowany, niezaimplementowany.**
  Włączenie go daje celowo błąd kompilacji: `dac_ext.cpp` jest zaślepką bez
  wybranego układu. 16-bitowe PWM daje około 50 µV na krok przy 3,3 V, blisko
  2,7×10⁻¹¹ względnie na oscylatorze 5,3 Hz/V; układ 18-bitowy z odniesieniem
  zaprojektowanym do tego zadania osiąga mniej więcej 17 µV, blisko 9×10⁻¹², bez
  opóźnienia filtru w pętli.

  Sprzętowe SPI nie jest potrzebne ani dostępne — SPI1 należy do TFT, a wszystkie
  piny SPI2 w tej obudowie są zajęte — ale DAC zapisuje się raz na sekundę, więc
  programowe kluczowanie kosztuje mikrosekundy. Proponowane piny PB0, PB2, PB4,
  dobrane tak, by ominąć PB6/PB7: te wyglądają na wolne, ale są domyślnymi pinami
  I2C1, które zajmuje `Wire.begin()`, a przetwornik tam rozwaliłby czujniki i
  wyświetlacz zegarowy.

### Zmienione
- **Wszystkie 23 wywołania `analogWrite(PIN_VCTL_PWM, ...)` przechodzą teraz przez
  `gpsdo_dac_write16()`.** Dodanie drugiej ścieżki wyjściowej przez edycję każdego
  z osobna prosiłoby się o przeoczenie jednego, a przeoczone miejsce to najgorszy
  rodzaj błędu tutaj: pętla sterowałaby poprawnie prawie zawsze i przeskakiwała
  przy każdym trafieniu w starą ścieżkę. Dodanie przetwornika sprowadza się teraz
  do wypełnienia jednej funkcji.

## [v1.01-rtos] — 2026-07-29

> **Buduj rdzeniem STM32duino 2.12.0 lub starszym.** Rdzeń 3.0.0 (23 lipca 2026)
> wdraża ArduinoCore-API, co usuwa `ltoa()` i zamienia `HardwareSerial` w
> interfejs abstrakcyjny — obu tu używamy — a co ważniejsze, pozostawia TFT_eSPI
> bez możliwości zainicjalizowania panelu (biały ekran, CLI działa normalnie).
> Dwa pierwsze są drobne i można je uwarunkować wersją; trzeci leży w bibliotece.
> Szczegóły w README.

Wydanie-kamień milowy: łączy gałąź trwałego zapisu we flash ringu z gałęzią
algorytmu 11 (LTIC-Lars). Algorytm 11 opiera się na oryginalnym kontrolerze GPSDO
z ciągłą pętlą PI autorstwa śp. **Larsa Waleniusa**, udostępnionym społeczności
time-nuts; został tutaj rozwinięty o poniższą auto-kalibrację i akwizycję, ku jego
pamięci.

### Dodane
- **Algorytm 11 „LTIC-Lars"** — pojedyncza ciągła pętla PI (bez maszyny stanów
  ACQ/DPLL/LOCK), dyscyplinująca OCXO z fazy sprzętowego TIC. Wybierany przez
  `LA 11`; trend LFQ (prowadzony częstotliwością) / LPH (faza) / LLK (lock).
  Strojony na żywo przez LG/LD/LTC/LFD/LTO/LPL/LPF/LTK/LTR.
- **Auto-kalibracja z CT dla algorytmu 11.** gain domyślnie 0 = auto: pętla
  wyprowadza skalę częstotliwości ze zmierzonego przez CT K (Hz na LSB PWM), tej
  samej stałej, której używa algo 10, więc jedno CT kalibruje też pętlę Larsa.
  Niezerowe LG nadpisuje skalą ręczną.
- **Akwizycja prowadzona częstotliwością** z dominującym, samohamującym członem
  proporcjonalnym, ograniczeniem kroku i anti-windup — zimny start dociąga bez
  ucieczki i bez oscylacji ±2 Hz obserwowanych podczas prac.
- **Pomost przechwytywania fazy picDIV**: gdy częstotliwość jest ustalona, ale
  faza wciąż railed, picDIV zostaje przezbrojony raz, by wprowadzić fazę w okno
  detektora, gdzie gałąź fazowa kończy lock.
- **Tuner: wersjonowany i dopasowany do firmware.** Narzędzia mają teraz
  TOOL_VERSION śledzące wydanie firmware, a tuner przy połączeniu odczytuje wersję
  z płytki: niezgodność jest zgłaszana na pasku stanu i w monitorze, zamiast
  objawiać się dziwnie czytanymi polami. Okno główne otwiera się zmaksymalizowane
  ze splashem na wierzchu, a w Windows konsola powstająca po kliknięciu skryptu
  jest minimalizowana na pasek zadań (tylko gdy należy do tunera — terminal
  otwarty przez operatora zostaje nietknięty).
- **Tuner: zakładka Help i splash startowy.** Tuner zyskał zakładkę Help z pełnym
  wykazem komend pogrupowanym tematycznie oraz trzysekundowy splash animujący dwie
  przesunięte w fazie sinusoidy zbiegające się w jedną — ta sama metafora locka co
  na ekranie startowym TFT (kliknięcie pomija). Przy połączeniu odczytuje też
  wszystkie grupy parametrów (LTIC, FA, PID 3-9, Lars) zamiast dwóch, a algorytm 11
  ma własną zakładkę obok algorytmu 10.
- **Trwały zapis algorytmu 11 we flash ringu.** Wszystkie parametry g_lars są
  zapisywane we flash ringu razem z ustawieniami LTIC (SETTINGS_VER 2); `ES LTIC`
  zapisuje oba. Nigdzie EEPROM — trwałość w 100% oparta na flash ringu.

### Zmienione
- **`LC` ostrzega, gdy uruchomione przed `CT`.** Te komendy nie są niezależne:
  `LC` potrzebuje nachylenia Hz na LSB, które mierzy `CT`, a bez niego przyjmuje
  wartość ogólną. Awaria jest cicha, nie oczywista — jedna płytka zgłosiła
  ns_per_volt 1592,8 przed `CT` i 921,2 po, różnica 1,7×, przy czym nic w pierwszym
  przebiegu tego nie sugerowało. `LC` mówi teraz o tym wprost i mimo to
  kontynuuje, a README podaje kolejność jednoznacznie.
- **Każde ustawienie mówi teraz, czy zostało zapisane.** Preferencje niedotykające
  pętli sterującej — strefa czasowa (`TZ`/`TO`/`LT`), offsety czujników (`PO`/`AO`)
  oraz flagi startu i survey-in (`WU`/`SPL`/`SV`) — zapisują się same, a odpowiedź
  podaje zapisaną grupę. Strojenie pętli pozostaje ręczne, a odpowiedź podaje
  dokładną komendę, która je utrwali, np. `[not saved — run 'ES LTIC' to keep it]`,
  więc grupy nie trzeba zgadywać. `SET_FLAGS` niesie `SAW` i `LRN` razem z flagami
  startu, więc auto-zapis utrwala także je; komunikat wymienia całą grupę, zamiast
  to ukrywać. Wartość odrzucona jest zgłaszana jako taka —
  `[not saved — value out of range; accepted range shown above]` — zamiast
  proponować komendę `ES` dla zmiany, która nie nastąpiła.
- **`LT` jest teraz trwałe.** Komenda była zaimplementowana, ale nie miała pola w
  bloku ustawień, więc wybór UTC/czas lokalny nie przeżywał restartu. Dodane do
  grupy strefy czasowej (SETTINGS_VER 4).
- **`CT` zapisuje teraz wynik automatycznie.** Tak jak `LC`, trzyminutowa
  kalibracja po powodzeniu wpisuje współczynniki do flash ringu, zamiast liczyć na
  to, że operator pamięta o `ES PID`. Zapisywana jest tylko grupa PID, więc
  strojenie pętli prowadzone równolegle pozostaje nietknięte.
- **Etykiety trendu algorytmu 11 przemianowane** na ACQ / PLL / LOCK, zgodnie ze
  słownictwem algorytmu 10, żeby wyświetlacze, CLI i tuner czytały się spójnie.
  Algo 11 pokazuje PLL tam, gdzie algo 10 pokazuje DPLL, co wciąż rozróżnia oba
  w logu.
- **Telemetria Learn pokazuje, co naprawdę steruje daną pętlą**: algo 11 pokazuje
  tryb gain / skalę / filtrowaną fazę, algo 10 swoją maszynę stanów, algo 3-9
  zachowują liczby LRN. qErr zostaje w każdej linii (wspólny dla obu gałęzi LTIC).
- **Komunikat CT** mówi teraz, że stroi algo 3-9 oraz LTIC 10 i 11.
- **Wrappery trwałości przemianowane** eeprom_* → persist_*, by odzwierciedlić, że
  zapis to flash ring, nie EEPROM; nazwy przestają wprowadzać w błąd.

### Naprawione
- **Survey-in nigdy nie przechodził w tło po resecie.** Licznik cierpliwości biegł
  od bootu hosta, ale odbiornik timingowy prowadzi survey nieprzerwanie przez reset
  MCU — ma własne zasilanie i własny stan, i zgłasza własny czas trwania. Każde
  przeprogramowanie zerowało więc licznik i dawało survey kolejny pełny limit na
  pierwszym planie, w nieskończoność. Zaobserwowane na stanowisku: odbiornik
  zgłaszał 4450 s survey przy 7 minutach pracy hosta, a komunikat o timeoucie nie
  padł ani razu. Deadline wygasa teraz, gdy przekroczy limit **którykolwiek** z
  dwóch zegarów, a komunikat mówi który.
- **Algorytm 10 mógł zamarznąć przy zdrowym dociąganiu.** Zabezpieczenie przed
  ucieczką wyzwalało się na samym zablokowanym detektorze plus dużym błędzie
  częstotliwości — a to jest normalny stan zimnego lub odległego OCXO na początku
  akwizycji, i zamrożenie w tym miejscu odcina jedyną drogę powrotu, bo to właśnie
  człon częstotliwościowy wciąga oscylator w okno detektora. Zaobserwowany przebieg
  przeszedł 3855 LSB w trakcie całkowicie zdrowego dociągania DPLL i został
  zamrożony w połowie. Oba zabezpieczenia wymagają teraz dodatkowo, by błąd
  przestał się poprawiać przez kilka cykli (LTIC_RUNAWAY_STALL) — co prawdziwa
  ucieczka przy złej polaryzacji wyzwala, a zdrowa akwizycja nigdy. Próg szyny
  znów pochodzi z kalibracji LC zamiast ze stałych 3,28 V pasujących do jednej płytki.
- **`LIV` było ograniczone do 30 s.** I CLI, i pętla przycinały interwał korekcji
  LOCK do 30, a pętla dla wartości spoza zakresu skakała na 5 s — więc prośba o
  wolniejszą pętlę po cichu dawała najszybszą. Przywrócone 1..600 s, z przycinaniem
  do najbliższej granicy. Miało to natychmiastowe znaczenie: tester porównujący
  LIV 30 z LIV 60 dostałby odrzucone 60.
- **Ustawienia w rzeczywistości nigdy nie były zapisywane.** Nagłówek slotu
  przechowywał długość danych w jednym bajcie, więc wszystko powyżej 255 B się
  zawijało: 324-bajtowy blok ustawień zapisywał się jako 68. Same dane trafiały do
  flash poprawnie, a CRC je obejmowało, więc nic nie wyglądało źle — ale każdy
  odczyt zwracał obciętą długość, zostawiając ogon wczytanego bloku jako to, co
  akurat leżało na stosie. Stamtąd wzięło się dziwne `temp_coeff=-1`, a gdy
  długość zaczęła być sprawdzana dokładnie, odczyt zaczął odrzucać rekord i płytka
  wstawała na wartościach domyślnych. Pole długości jest teraz 16-bitowe (nagłówek
  slotu 4 B → 6 B, dane 506 B → 504 B), a magic pierścienia podbity, żeby starszy
  pierścień sam się przeformatował, zamiast dekodować się jako śmieć. Dotyczyło to
  gałęzi flash-ring od początku — blok GML-a miał już 292 B, też ponad limit.
- **Przepełnienie stosu przy zapisie do flash ringu.** Zapis ustawień potrzebuje
  około 1,4 KB stosu — `fr_write()` buduje 512-bajtowy obraz slotu plus
  512-bajtową kopię do weryfikacji, a `settings_store` dokłada blok ~324 B — a
  zadanie CLI miało 1 KB, zadanie kontrolne 1,5 KB. Zadanie CLI wychodziło poza
  swój stos i nadpisywało sąsiada: płytka drukowała potwierdzenie zapisu i
  zawieszała się z zamrożonym wyświetlaczem. Oba stosy podniesione z zapasem
  (CLI 1 KB → 3 KB, kontrolne 1,5 KB → 3,25 KB; 4 KB RAM więcej ze 128 KB).
  Zagrożenie istniało przed pracą nad auto-zapisem — `ES` był równie narażony —
  ale auto-zapis sprawił, że łatwo je było trafić.
- **`EW` podawał zły sektor flash.** Pierścień od zawsze mieszka w sektorze 7
  (0x08060000, ostatni sektor, żeby firmware zachowało maksymalną ciągłą
  przestrzeń poniżej), ale komunikat `EW` miał na sztywno „sector 6, 0x08040000" —
  jedyne miejsce, w które zagląda operator, było jedynym, które kłamało. Komunikat
  czyta teraz adres z implementacji przez nowe funkcje `flash_ring_sector_no()` /
  `flash_ring_base_addr()`, więc nie może się już rozjechać. Dokumenty bring-up
  miały te same nieaktualne liczby i zostały poprawione w trzech językach: limit
  firmware to 393216 B (384 KB), nie 262144 B, a zakres kasowania J-Link dla
  wyczyszczenia pierścienia to 0x08060000-0x0807FFFF, a nie zakres sektora 6,
  który zostawiłby pierścień nietknięty.
- **LC nie wyrzuca już własnego dorobku.** Pętla zerująca tempo kończyła po trzech
  próbach i, jeśli nie trafiła jeszcze w pasmo akceptacji, wracała do
  `saved_pwm + offset` — co zakłada, że zapisany PWM leży w punkcie locka.
  Uruchomiona, zanim oscylator jest blisko 10 MHz, to założenie jest fałszywe:
  obserwowany przebieg zbiegał -244, -57, -16 ns/s (krok od pasma), po czym to
  odrzucał i próbkował przy PWM dającym -244 ns/s, gdzie faza przebiega całe okno
  detektora między publikacjami. Każde zbrojenie picDIV lądowało na szynie i
  kalibracja przerywała. Pętla ma teraz sześć prób, a gdy się wyczerpią, zachowuje
  wysterowany PWM zamiast wracać do początku.
- **Przywrócone FA / FAD / FAL.** Okno uśredniania członu tłumiącego per stan
  (i człon `damp_e_freq`, który zasila w algorytmie 10) istniało w v0.97, ale nie
  w gałęzi flash-ring, więc przepadło przy merge'u. Przywrócone i zapisywane teraz
  we flash ringu zamiast w EEPROM.
- **Rekordy ustawień mają sprawdzaną długość.** `settings_recall` i
  `settings_save_partial` przyjmowały dowolny rekord od dwóch bajtów wzwyż do
  bloku na stosie, więc rekord krótszy od bieżącej struktury zostawiał ogon jako
  śmieć ze stosu — a zapis częściowy wpisywał ten śmieć z powrotem. Oba zerują
  teraz blok i wymagają dokładnego rozmiaru.
- **settings_store.cpp kompiluje się teraz.** Czytał trzy globalne, których nie
  widział — g_pressure_offset, g_altitude_offset (definiowane w gpsdo_control.cpp,
  bez własnego nagłówka) oraz g_qerr_enable (deklarowany w ubx_timtp.h, który nie
  był zaincludowany). Dodano include i dwa lokalne externy, zgodnie ze wzorcem
  używanym w reszcie projektu.
- **Zaimplementowana komenda LT.** Pomoc od zawsze dokumentowała `LT 0|1`, a
  ścieżki wyświetlania i raportów od zawsze czytały g_show_local_time, ale handler
  w CLI nigdy nie powstał — więc komenda po cichu nic nie robiła. Teraz przełącza
  i raportuje UTC / czas lokalny, tak jak obiecuje pomoc.
- **dph na serialu zgadza się teraz z panelem.** Wiersz TFT odejmował sawtooth
  odbiornika, a raport szeregowy nie — więc ten sam moment czytał się inaczej na
  obu, o cały sawtooth (~±10 ns na LEA-6T, więcej na M8T). Ścieżka szeregowa też
  go teraz odejmuje, zgodnie z tym, co jej własny komentarz już deklarował.
- **CR (zimny restart) naprawdę czyści teraz ring.** persist_erase() woła nowy
  flash_ring_wipe(), który fizycznie eraseuje i reformatuje sektor ringu, więc
  zimny restart faktycznie wraca do domyślnych, zamiast tylko oznaczać stan jako
  nieaktualny.

## [v0.95-rtos] — 2026-07-16

### Dodane
- **Strefy czasowe z DST, w całym świecie.** `TZ Adelaide` wystarczy, żeby
  zegar był poprawny — łącznie z offsetem pół godziny i DST półkuli
  południowej. Same nazwy miast są akceptowane: są unikalne w całej bazie
  IANA, więc region jest opcjonalny (`TZ Australia/Adelaide` też działa),
  a wielkość liter nie ma znaczenia.

  Regułę można też wpisać w całości: `TZ ACST-9:30ACDT,M10.1.0,M4.1.0/3`. Ta
  forma ma znaczenie, gdy rząd zmieni przepisy, a firmware jeszcze o tym nie
  wie — użytkownik poprawi to z CLI, zamiast czekać na wydanie.

  Wbudowane 407 stref i 88 reguł, generowane z systemowej tzdata przez
  `tools/gen_tz_table.py`. Pełna baza IANA to ~2 MB, czterokrotność całego
  flasha tego MCU, a jej prawdziwa wartość polega na aktualizacjach kilka razy
  w roku — z czego GPSDO bez internetu i tak nie skorzysta. String POSIX TZ, do
  którego sprowadza się każda strefa, ma 4–44 bajty i niesie to samo
  zachowanie na dziś, więc to on jest przechowywany. Koszt: ~7 KB flasha.
- **`H TZ`** — pierwsza strona pomocy dla pojedynczej komendy. `TZ` przyjmuje
  dwa całkiem różne argumenty i ta różnica ma znaczenie, więc dostaje własną
  stronę zamiast ciasnej linijki w głównej liście.
- **`TO` przyjmuje teraz minuty**: `TO 9:30`, `TO -3:30`, `TO 5:45`. Same
  godziny nadal działają.
- **Vcc na ekranie (480×320).** Prośba Dana Wieringa, obok Vdd. Szyna 5 V była
  już mierzona, ale nie miała gdzie trafić — każda komórka w obu kolumnach jest
  zajęta. Komórka `Alt` ma ~134 px luzu za wysokością, więc oddaje prawą połowę,
  a pola zostały przy okazji przegrupowane: `qErr` idzie w górę obok `Alt` (to
  raport odbiornika o własnym 1PPS, więc jego miejsce jest przy danych z fixa),
  a `Vcc` zajmuje miejsce zwolnione przez `qErr` obok `Vdd` — napięcia razem.
  `Vdd` odzyskuje drugie miejsce po przecinku, które oddawało wyłącznie po to,
  by zrobić miejsce dla `qErr`.

  Oba tylko na 480. Na 320 `Alt` i `qErr` chcą ~168 px, a komórka ma 148, więc
  ten panel zostaje przy starym układzie.

### Naprawione
- **Zgłoszone przez Dana Wieringa: auto-strefa nie łapała DST w Australii
  Południowej.** Dwa osobne błędy, z czego widoczny był jeden. `TO A` zgaduje
  strefę z długości geograficznej i stosuje europejską regułę DST, więc poza
  Europą nie dawało DST w ogóle — to był zgłoszony objaw. Ale zwracało też
  całe godziny, a Adelaide to UTC+9:30, więc zegar był o pół godziny obok
  nawet zimą, przy naprawionym DST. Indie (+5:30), Nepal (+5:45), Nowa
  Fundlandia (−3:30) i Chatham (+12:45) miały ten sam cichy błąd.

  `TZ <strefa>` rozwiązuje oba. `TO A` zostaje bez zmian — jest poprawne
  w większości Europy i nie wymaga konfiguracji — ale teraz mówi wprost,
  czego nie potrafi.
- **Częstotliwość skakała na boki na panelu 320×240.** v0.94 usunęło szerokość
  pola `dtostrf` w przekonaniu, że font monospace i tak trzyma cyfry
  w kolumnach. Trzyma — ale string, który traci znak, wciąż jest centrowany na
  nowo, co przesuwa każdy glif o pół znaku. To szerokość pola sprawia, że
  *string* ma stałą długość, i wróciła — jest tam od v0.89. Panel 480×320
  nietknięty: kotwiczy odczyt prawą krawędzią, co jest zweryfikowane na
  sprzęcie.
- **Boczne szyny znikały przy częstotliwości.** Sprite częstotliwości czyści
  całe swoje pasmo przed rysowaniem, a rysował tylko linię separatora nad sobą
  — więc szyny z początkowego layoutu były zamazywane z tego pasma przy
  pierwszej aktualizacji i ramka wyglądała, jakby nie dochodziła do linii
  nagłówka. Sprite niesie teraz także szyny. Oba panele.
- **Vdd pokazywało się wyłącznie przy zbudowanym LTIC.** Siedzi w wierszu fazy,
  a cały wiersz był pod `#ifdef GPSDO_LTIC` — więc płytka bez TIC nie widziała
  własnej szyny 3.3 V, bez lepszego powodu niż to, gdzie akurat napisano to pole.
  Szyny są teraz poza tym warunkiem: `Vcc` i `Vdd` pokazują się niezależnie od
  sprzętu, a pod LTIC zostaje samo pole fazy — bez niego lewa połowa wiersza jest
  po prostu pusta. `qErr` też zostaje pod warunkiem, bo pojawia się wyłącznie
  przy algo 10.
- **`CT` pokazywało „Tune 0s" przez cały przebieg.** Ustawiało flagi
  kalibracji, ale nigdy nie zasiewało licznika, w przeciwieństwie do `C` i
  `LC`. Trzy punkty po `OCXO_CALIB_SECS`, czyli 185 s.
- **`qErr` przesuwało się na panelu 480×320.** Przy kotwicy z lewej pole rosło
  w prawo wraz ze zmianą szerokości wartości i „ns" wędrowało tam i z powrotem.
  Zakotwiczenie całego stringu z prawej naprawiło jednostkę, ale w zamian
  ciągnęło etykietę `qErr:` razem z cyframi. Etykieta i wartość to teraz dwa
  osobne pola: etykieta dosunięta do lewej krawędzi slotu, wartość trzyma
  kotwicę z prawej, żeby jednostka stała, a zmienia się wyłącznie odstęp między
  nimi — czyli tak, jak od zawsze zachowuje się wiersz `Vph`/`dph`.

### Zmienione
- **`dph` podawało pewną siebie liczbę długo po tym, jak detektor przestał
  mierzyć.** `ns_per_volt` to nachylenie LOKALNE, odczytywane wokół kotwicy,
  którą LC stawia na 0.632·Vsat; sama rampa to `V = Vsat·(1 − e^(−φ/τ))`, więc z
  dala od kotwicy krzywa płaszczeje i liniowy odczyt zaniża fazę. Powyżej Vsat
  nie ma już żadnego odczytu — impuls stopu minął okno i kondensator ładuje się
  dalej do szyny zasilania. Wyświetlacz zameldował ten stan dwukrotnie jako
  niewzruszone „+1561 ns" i za każdym razem kosztowało to pomiar, zanim ktoś
  spojrzał na napięcie obok. `dph` pokazuje teraz `ovf` poza pasmem 15–85% Vsat,
  a stojące obok `Vph` mówi, którym końcem wyjechało.

  Vsat nie jest nigdzie zapisywane — LC je dopasowuje, stawia kotwicę i wyrzuca
  — ale kotwica jest z definicji na 0.632·Vsat, więc `zero_offset` je odzyskuje.
  Na tej płytce wychodzi 2.91 V, co zgadza się z 2.93 V, które komentarze
  kalibracji podają dla niej.

  Osobno warte odnotowania: własna ochrona pętli przed ucieczką (`railed_now`)
  testuje twardo wpisane 3.28 V. Przy detektorze nasycającym się koło 2.9 V nie
  ma prawa zadziałać, więc pasmo między ~2.9 a 3.28 V jest nasycone z punktu
  widzenia sprzętu i zdrowe z punktu widzenia pętli. Tego ta zmiana nie rusza.
- **`dph` na ekranie nigdy nie miało odjętego sawtootha.** Wyświetlacz liczył
  fazę własną ścieżką — napięcie, środek, `ns_per_volt` — i pomijał korektę,
  którą pętla stosuje w `ltic_phase_error_ns()`. Czyli algo 10 sterowało na
  fazie skorygowanej, a pokazywało nieskorygowaną; różnica to cały sawtooth
  odbiornika: zmierzone na sprzęcie ~14 ns rozrzutu 1σ na skądinąd płaskim
  odczycie. Teraz wyświetlacz też go odejmuje.

  Najbardziej znaczy to poza algo 10. Algorytmy 3–9 nigdy nie wołają fazowej
  ścieżki pętli, więc `dph` było ich jedynym widokiem na prawdziwą fazę — i to
  tym zaszumionym. A to właśnie tam TIC jest cenny: rozróżnia odchyłkę
  częstotliwości na poziomie ~5e-11 w 100 s, podczas gdy licznik cykli potrzebuje
  1000 s na 1e-10. Pole `qErr` i pozycja `qErr=` w raporcie szeregowym też nie są
  już bramkowane na algo 10: to, co zostało odjęte, musi być widoczne, inaczej
  liczby nie da się potem sprawdzić.
- **Każda kolumna ma jedną linię wyrównania z prawej (480×320).** Lewa kończy
  się tam, gdzie „hPa" w wierszu BMP, prawa tam, gdzie „ns" w wierszu fazy — bo
  to najszersze i najstabilniejsze stringi w każdej z nich. `Vct`, `% rH` i prąd
  z INA są teraz zakotwiczone do tych linii, zamiast każdy kończyć się tam, gdzie
  akurat wyczerpie się jego tekst — przez co krawędzie kolumn były trzema
  rozjazdami po kilka pikseli. `PWM:` i `INA:` zachowują etykiety przy lewej
  krawędzi kolumny, więc oba wiersze musiały stać się dwoma polami zamiast
  jednym stringiem.

  Linie są mierzone przez `textWidth()` przy pierwszym użyciu, a nie wpisane jako
  stałe: każda wartość w tych wierszach ma stałą szerokość, więc każda krawędź
  jest stałą — ale stałą wynikającą z metryk glifów fontu, a tego nie warto
  zgadywać. Paddingi też wyprowadzone z pomiaru, więc pola kafelkują wiersz
  niezależnie od tego, ile wyjdzie.
- **Wiersze sensorów grupowane kolumnami, nie sensorami (480×320).** BMP i AHT
  wypełniają teraz lewą kolumnę, a pola elektryczne prawą — odczyt fazy na
  górze, szyny zasilania bezpośrednio pod nim. `AHT` i `Vph`/`dph` zamieniły się
  miejscami. Przeniesienie pola fazy do węższej prawej kolumny kosztowało jedną
  spację przed `dph:`; jego padding jest wymierzony na najszerszy możliwy string,
  nie na kolumnę, więc kurcząca się wartość nie zostawi ogona.
- **`dPh:` to teraz `dph:`**, pasujące do `Vph:` obok. Zmienione na TFT i w
  raporcie szeregowym razem — te etykiety miały się zgadzać, więc zmiana tylko
  jednej pogorszyłaby sprawę, zamiast ją poprawić.
- **Powiadomienie survey-in przeniesione z belki górnej na belkę statusu.**
  Pulsowało między nazwą programu a zegarem i na panelu 480 nie pojawiało się
  w ogóle — usterka, która przetrwała każde czytanie kodu i kilka pewnych
  siebie błędnych diagnoz. Zamiast polować dalej, powiadomienie dopisuje się
  teraz do tego, co belka statusu i tak mówi: `DISCIPLINED  FIX OK SURVEY`,
  albo `SV` na 320, gdzie pełne słowo wyszłoby poza pasek.

  Belka jest lepszym miejscem niezależnie od błędu. Przemalowuje całe swoje tło
  przed rysowaniem, więc słowa nie utnie padding sąsiada — czego slot w nagłówku
  nie potrafił zagwarantować; jest jedynym miejscem na ekranie, gdzie oko i tak
  szuka stanu; a stojąc tam nie musi migać, żeby je zauważyć, więc pulsowanie
  też zniknęło.

  Warunek bez zmian, bo nigdy nie był problemem: napis pojawia się po timeoucie
  monitora survey-in, gdy odbiornik nadal go prowadzi, i gaśnie z chwilą
  wejścia w Time Mode.
- `g_time_offset` (int8, godziny) to teraz `g_time_offset_min` (int16, minuty),
  z jednym zapisującym. `g_tz_auto` (bool) stało się `g_tz_mode` (ręczny /
  auto-EU / POSIX): każda komenda ustawia tryb, więc nie ma pół-stanu, w
  którym jeden mechanizm jest skonfigurowany, a inny go po cichu nadpisuje.

### EEPROM
- Blok stref przeniesiony na `[234..284]`: tryb, ręczny offset w minutach i
  reguła POSIX jako tekst.
- **Istniejące ustawienia są migrowane automatycznie — bez factory reset.**
  EEPROM sprzed v0.95 nigdy nie był zapisywany powyżej `[233]`, więc blok
  odczytuje się jako skasowany flash; to jest znacznik, a stara para
  `[9]`/`[142]` jest przenoszona (godziny × 60 to dokładnie to, co znaczyła).
  Sygnatura bez zmian.
- **Ale powrót do starszej wersji jest jednokierunkowy.** Starsze bajty `[9]`
  i `[142]` są nadal zapisywane, więc v0.94 wgrane na płytkę z v0.95 odczyta
  sensowny offset w całych godzinach — ale reguły `TZ` nie da się tam
  zapisać i zostanie utracona.

### Dokumentacja
- Przeniesiona do [`doc/`](../doc/), pliki angielskie dostały sufiks `_EN`, żeby
  wszystkie trzy języki nazywały się tak samo. Główny `README.md` to teraz krótki
  indeks — GitHub renderuje go na stronie projektu i stamtąd prowadzi do `doc/`.
- Przewodniki uruchomienia flash-ringu były sierotami: nic do nich nie linkowało
  i one do niczego. Mają teraz tę samą nawigację językową co reszta.
- Ich liczba budżetu flasha była przestarzała o pięć wersji (~170 KB przy v0.90).
  Teraz mówi 216976 B (212 KB) przy v0.95, ~44 KB zapasu poniżej ringu na
  0x08040000 — zmierzone, nie szacowane. Ta liczba jest całym sensem tego
  sprawdzenia, więc nie powinna gnić. Przewodnik ostrzega też, że procent z IDE
  liczy od pełnych 512 KB i wygląda dużo różowiej niż prawda: „41%" to w
  rzeczywistości 83% tego, co firmware może wykorzystać.

### Uwagi
- `tz_table.h` jest generowany. Uruchom `tools/gen_tz_table.py` ponownie przy
  aktualizacji tzdata; reguła zapisana w EEPROM przetrwa regenerację.
- Africa/Casablanca i Africa/El_Aaiun degradują się do offsetu standardowego
  z ostrzeżeniem: ich DST zależy od ramadanu, czego format POSIX w ogóle nie
  wyraża. Każda inna strefa w obecnej tzdata rozstrzyga się w pełni.

---

## [v0.94-rtos] — 2026-07-15

### Naprawione
- **Pole częstotliwości na 320×240 wciąż rysowało się fontami GFX.** v0.93
  cofnęło mały panel na klasyczne fonty, ale poprawka trafiła tylko do ścieżki
  rysowania bezpośredniego — a ta nigdy się nie wykonuje, bo sprity tworzone są
  na *obu* panelach, nie tylko na 480×320. Gałąź sprite'owa miała nadal
  zaszyte `GF_FREQ`/`GF_STATUS`, więc odczyt (i `no signal`) dalej renderował
  się we FreeMono. Teraz idzie przez te same makra `TFT_FONT_*` co reszta.
- **Częstotliwość drgała w bok na panelu 480×320.** Odczyt był centrowany, więc
  każda zmiana długości napisu ruszała wszystkimi znakami: okno uśredniania
  zmienia liczbę miejsc po przecinku, a 10000000.0000 → 9999999.9999 gubi cały
  znak, przy czym centrowanie rozkładało tę różnicę na oba końce. Odczyt jest
  teraz zakotwiczony prawą krawędzią na x=464 — dobrane tak, by nominalne
  `10000000.0000 Hz` (16 znaków × 28 px stałej szerokości = 448 px) nadal
  wypadało idealnie na środku, z 16 px powietrza po obu stronach. „Hz" już się
  nie rusza; ruszają się tylko cyfry. Komunikaty statusu zostają wycentrowane —
  używają proporcjonalnego fontu, gdzie nie ma kolumn do wyrównania.

### Zmienione
- **Ramka jest biała na obu panelach.** Poza ujednoliceniem z dużym panelem, to
  właśnie pozwala 1-bitowemu sprite'owi danych nieść ramkę samodzielnie: ten
  sprite ma dokładnie dwa kolory (biel i tło), więc granatowej ramki nie dało
  się w nim narysować i trzeba ją było domalowywać na panelu po każdym pushu.
  Biel oznacza, że ramka i tekst wychodzą teraz razem, jednym atomowym
  transferem, na obu rozmiarach. Separator pod nagłówkiem przeniósł się do
  sprite'a częstotliwości z tego samego powodu (jego paleta 4-bit ma już biel).
- **Splash nie używa już fontów GFX.** Był ostatnim bastionem GFX na małym
  panelu, co oznaczało, że każdy przechodzący z v0.92 musiał dodać
  `LOAD_GFXFF` do `User_Setup.h` albo patrzeć, jak podtytuł zwija się do samego
  „p" — zagadkowa awaria za kosmetyczny zysk. Podtytuł używa teraz klasycznego
  fontu 4 (który ma pełny alfabet — to fonty 6/8 są bez liter), a kredyty fontu
  1 na obu panelach. **Wersja 320×240 potrzebuje teraz tylko `LOAD_GLCD`,
  `LOAD_FONT2` i `LOAD_FONT4`**; `LOAD_GFXFF` jest wymagane wyłącznie dla
  480×320. Osierocone makra `GF_TITLE`/`GF_SUB`/`GF_CREDIT` i martwa gałąź 320
  w bloku `GF_*` znikają razem z tym.
- Napisy paska statusu siedzą 2 px niżej na panelu 320×240. Są pisane samymi
  kapitalikami, więc pole na dolne wydłużenia glifów jest puste, a centrowanie
  geometryczne czyta się jako za wysokie; przesunięcie centruje to, co oko
  faktycznie widzi. Panel 480×320 bez zmian.
- Podbicie wersji do v0.94-rtos, wraz z nagłówkami plików (które wciąż mówiły
  v0.92).

## [v0.93-rtos] — 2026-07-14

### Naprawione
- **Odliczanie szło wolniej niż zegar.** Rozgrzewka OCXO i kalibracje mierzyły
  sekundy przez `vTaskDelay(1000)`, które śpi *przez* sekundę, a nie *do*
  następnej — więc odczyty ADC, wydruki na serial i każde wywłaszczenie
  doliczały się na wierzch, a pokazywana liczba zostawała w tyle za realnym
  czasem (tym bardziej, im bardziej obciążony system). Oba używają teraz
  `vTaskDelayUntil`, które pochłania czas pracy i utrzymuje każdy krok jako
  prawdziwą sekundę. Licznik kalibracji zatrzymywał się też na 1 zamiast dojść
  do 0.
- **Survey-in, który przeżyje okno monitorowania, nie jest już niewidoczny.**
  Gdy zadziała zabezpieczający timeout, firmware przestaje odpytywać, ale
  odbiornik dalej prowadzi survey („continuing anyway" w logu) — a skoro pasmo
  częstotliwości wraca do pokazywania częstotliwości, nic na ekranie o tym nie
  mówiło. Wolno pulsujący `SURVEY` siedzi teraz w nagłówku między wersją a
  zegarem i gaśnie sam, gdy odbiornik zgłosi Time Mode (`HDOP: TIME`), co jest
  prawdziwym sygnałem zakończenia survey-in.
- **`qErr` zostawiał resztki znaków na panelu ILI9488** (widoczne jako
  `qErr: -1.6 nsss`). Padding tekstu pola wynosił 55 jednostek autorskich
  (~82 px), a najszersza wartość `qErr: -21.3ns` potrzebuje ~104 px w FreeSans
  9pt — TFT_eSPI przemalowuje tło tylko pod paddingiem, więc ogon poprzedniego,
  dłuższego napisu zostawał. Padding poszerzony do 75 jednostek (~112 px), co
  pokrywa tekst i nadal omija zakotwiczone do prawej pole `Vdd`.
- **Vctl / Vcc / Vdd pokazywały 0,000 V przez całą rozgrzewkę OCXO.** Te średnie
  ADC są próbkowane w głównej pętli zadania sterowania, ale `do_warmup()`
  wykonuje się *przed* wejściem do tej pętli i tylko spał — więc nic ich nie
  wypełniało. Odliczanie rozgrzewki próbkuje teraz te same trzy kanały co
  sekundę, tak jak już robi `wait_secs_pwm()` podczas kalibracji.
- **Odczyt częstotliwości siedział na prawo od środka i skakał w bok.** Wartość
  była formatowana przez `dtostrf(..., 14, ...)`, dopełniając ją z lewej do 14
  znaków; `MC_DATUM` centrował potem napis *razem* z tymi niewidocznymi
  spacjami, więc widoczne cyfry siedziały ~40 px na prawo od środka — a ponieważ
  liczba spacji zmienia się z oknem uśredniania (1–4), odczyt przesuwał się przy
  każdej zmianie precyzji. Szerokość pola usunięta: `GF_FREQ` to FreeMonoBold,
  który i tak trzyma cyfry w stałych kolumnach, więc dopełnianie nic nie dawało.
  Wraz z nim znika obejście z końcową spacją na panelu 480.

### Zmienione
- **Panele 320×240 wracają do klasycznych fontów na ekranie roboczym.** v0.92
  przeniosło wszystkie panele na fonty GFX; na 480×320 to była wyraźna wygrana,
  ale przy 320×240 kroje proporcjonalne są za szerokie dla layoutu ułożonego
  wokół fontów numerycznych — wartości uciekały poza swoje kolumny do sąsiedniej,
  a środkowy separator przecinał to, co wystawało. Nie było też mniejszego kroju
  do odwrotu (FreeSans zaczyna się na 9 pt; niżej jest tylko nieczytelny
  TomThumb 3×5). Mały panel używa teraz fontu 2 na nagłówek i siatkę, fontu 4 na
  pasek statusu i fontu 1 ×3 (stała szerokość) na częstotliwość, a splash
  zostaje na GFX na obu panelach. Makra `TFT_FONT_*` wybierają to na etapie
  kompilacji — nadal jeden layout, nie dwa. Środkowy separator kolumn jest teraz
  tylko na 480 (na 320 nie ma na niego miejsca), a ramka wraca na małym panelu
  do granatu.
- **Żywe obszary wyświetlacza są podwójnie buforowane jako sprity.** Nagłówek,
  pasmo częstotliwości i obszar danych są renderowane do `TFT_eSprite` w RAM i
  wypychane na panel jednym ciągłym transferem SPI, zamiast kasowania panelu
  przez `setTextPadding` i rysowania na wierzchu. To kasowanie-a-potem-rysowanie
  było widoczne jako migotanie raz na sekundę, zwłaszcza na panelu 480×320,
  gdzie czyści 2,4× więcej pikseli. Palety trzymają koszt nisko (4-bit
  nagłówek/freq, 1-bit dane; ~25 KB łącznie na dużym panelu). Jeśli
  `createSprite()` zawiedzie przy pofragmentowanej stercie, każde pasmo wraca do
  rysowania bezpośredniego — migotanie wraca, ale nic się nie psuje; log
  startowy mówi, która ścieżka działa.
- **Komunikaty statusu piszą się teraz pełnymi słowami i nazywają, która
  kalibracja trwa.** `WARMUP 285s` → `OCXO warmup 285s`, `SVIN 120s 5m` →
  `Survey 120s +/-5m`, a niejednoznaczne `CAL 245s` staje się `Calibrate`,
  `Tune` lub `LTIC cal` — C, CT i LC trwają bardzo różnie, więc samo odliczanie
  niewiele mówiło operatorowi. Oba panele. Uwaga: obie liczby są innego rodzaju:
  rozgrzewka i kalibracje odliczają w dół, a survey-in liczy w górę (odbiornik
  raportuje czas, który minął, a zakończenie zależy też od dokładności, więc
  liczba „pozostało" byłaby zgadywanką).
- **`SPI_FREQUENCY 40000000` jest teraz udokumentowanym ustawieniem** (w README
  było 27 MHz, podczas gdy `gpsdo_config.h` mówił już 40). SPI1 w F411 kończy
  się na 50 MHz, więc 40 zostawia zapas; ma to znaczenie głównie na panelu
  480×320, gdzie push sprite'a to jeden transfer, którego czas skaluje się z
  zegarem. Zejdź do 27 MHz, jeśli długie przewody połączeniowe zaczną
  bruździć.
- **Wiersze kredytów na splashu dostały większą interlinię na panelu 480×320.**
  Autorski odstęp 12 jednostek skaluje się tam do zaledwie ~16 px, a kredyty to
  FreeSans 9pt (~13 px wysokości), więc oba wiersze zlewały się optycznie. Duży
  panel używa teraz odstępu 16 jednostek (~21 px, interlinia ~1,6×); panel
  320×240 zostaje przy 12, co pasuje do jego fontu 6×8.
- `dPh:` i `qErr:` na ILI9488 tracą spację przed jednostką `ns`.
- Podbicie wersji do v0.93-rtos.

## [v0.92-rtos] — 2026-07-12


### Zmienione
- **Uproszczony splash i dopracowane proporcje ekranu pracy.** Duży zielony
  tytuł „GPSDO" usunięto ze splashu startowego; podtytuł „GPS Disciplined OCXO"
  jest teraz podniesiony na górę, jak w oryginalnym układzie 320×240. Na ekranie
  pracy tekst nagłówka zmniejszono do rozmiaru fontu danych, dolny pasek statusu
  zmniejszono o połowę wysokości z mniejszym fontem statusu, a odzyskane miejsce
  poszło na szersze odstępy między wierszami telemetrii (row pitch 17→20
  authored), żeby siatka „oddychała". Font danych zostaje FreeSans 9pt.
- **Cały tekst TFT przeniesiony na fonty Adafruit GFX (GFXFF).** Nagłówek, duży
  odczyt częstotliwości, siatka danych, pasek statusu oraz tytuł/podtytuł
  splashu rysowane są teraz fontami FreeSans / FreeMono zamiast klasycznych
  numerycznych fontów GLCD. Naprawia to długotrwały błąd, w którym napisy
  literowe rysowane fontami numerycznymi (6/8, zawierającymi tylko
  `0-9 . : - a p m`) zwijały się do pojedynczego znaku — najwyraźniej podtytuł
  splashu „GPS Disciplined OCXO" renderowany jako samo „p" oraz pusty napis na
  kolorowym pasku statusu. Warstwa fontów per rola i per panel (`GF_DATA` /
  `GF_HEAD` / `GF_STATUS` / `GF_TITLE` / `GF_SUB` / `GF_FREQ` w
  `gpsdo_config.h`) dobiera automatycznie FreeSans 9/12 pt, FreeSansBold
  12/18/24 pt oraz FreeMonoBold 18/24 pt dla paneli 320×240 i 480×320, więc ten
  sam kod układu obsługuje oba. Duża częstotliwość używa FreeMonoBold, aby jej
  cyfry pozostały stałej szerokości i nie przeskakiwały przy zmianie wartości.
- **Wymaga `#define LOAD_GFXFF` w `User_Setup.h`** (patrz README). Stare linie
  `LOAD_FONT2/4/6/8` nie są już potrzebne; `LOAD_GLCD` pozostaje tylko dla dwóch
  drobnych linii autorskich na splashu.
- **Układ ekranu pracy przeliczony geometrycznie pod 480×320.** Granice pasów
  (częstotliwość, siatka, sensory, status) przeliczone tak, aby wyższe wiersze
  fontów proporcjonalnych nigdy nie przecinały separatora na żadnym panelu, obie
  kolumny danych wypełniają pełną szerokość z delikatnym separatorem środkowym,
  a pasek statusu wypełnia cały pas do dołu ekranu (brak martwego paska koloru
  pod napisem). Wartości siatki są kotwiczone prawym datum, więc zmienne
  szerokości pozostają przypięte zamiast dryfować. Zweryfikowane na panelu
  ILI9486 480×320.

### Naprawione
- **Usunięto nieaktualne „jeszcze nie zaimplementowane / phase A" z CLI i
  telemetrii.** `LA` z błędną wartością pisało „0..9 (10=LTIC, not yet
  available)", `LL` drukowało „(loop not yet implemented — phase A)", a
  pomoc/komentarze wciąż opisywały algo 10 jako niezaimplementowany podgląd.
  Algorytm 10 dyscyplinuje pętlę od wielu wydań; wszystkie te miejsca opisują
  teraz działającą 3-stopniową pętlę fazową ACQ→DPLL→LOCK. (`Vdd:` na TFT
  dostało też spację przed wartością, dla spójności z innymi etykietami.)
- **Animacje spinnera LED (rozgrzewka / survey-in / kalibracja) chodziły ~5× za
  wolno i skakały.** Task wyświetlacza budzi się na powiadomienie PPS 1 Hz, ale
  spinnery zmieniają klatkę co 200 ms — więc przy budzeniu co 1100 ms
  przesuwały się tylko raz na sekundę. Task budzi się teraz ~co 150 ms gdy
  animacja jest aktywna (a poza tym trzyma wolne 1100 ms, bo zegar i tak zmienia
  się raz na sekundę). Żeby szybsze budzenie nie przepychało identycznych
  segmentów po programowo bit-bangowanym TM1637 (~5–8 ms na zapis), mały cache
  zapisu (`tm_set`) pomija transfer gdy wzorzec się nie zmienił. To poprawka
  szeregowania/cache — bez DMA; DMA zostaje osobnym przyszłym krokiem dla ścieżki
  TFT SPI.
- **Podniesiona klamra tłumienia nie działała po ponownym wgraniu — lock
  oscylował LOCK↔DPLL.** Mnożnik tłumienia jest zapisywany w flash ring (dane
  żywe) i odtwarzany przy starcie. Flash zapisany przez build ze starą klamrą
  0,30 odtwarzał więc damp = 0,30 nawet po podniesieniu klamry do 0,45, a
  ponieważ damp adaptuje się tylko na przejściach cyklu granicznego, zostawał
  tam zablokowany — pętla działała z 30% mocą korekcji, faza rosła ponad próg
  locka, a pętla skakała LOCK↔DPLL co ~30 s (widoczne na sprzęcie). Odtworzony
  damp jest teraz clampowany do bieżącego legalnego zakresu przy wczytaniu
  (flash ring i EEPROM), więc ponowne wgranie działa od razu. Klamry damp
  przeniesione do wspólnego nagłówka, by pamięć i learner się zgadzały.
- **TFT pokazuje teraz qErr, a faza dostała etykietę `dPh:`.** Na algo 10 z
  aktywnym SAW prawe pole wiersza czujników zaczyna od piły odbiornika
  `qErr:…ns`, a dalej `Vdd:` skrócone do 1 miejsca. Oba są rysowane osobno — qErr do lewej,
  Vdd zakotwiczone do prawej krawędzi ekranu — więc Vdd nie przesuwa się już
  w bok, gdy qErr zmienia szerokość. Przy SAW wyłączonym pokazywane jest samo
  Vdd z pełną precyzją, nadal przy prawej krawędzi. Faza LTIC po lewej ma
  etykietę `dPh:±…ns` (bez spacji po `Vph:`) dla czytelniejszego, spójnego
  odczytu; raport szeregowy używa tej samej etykiety `dPh:` po `Vphase:`, więc
  oba są zgodne. qErr i dPh używają pola o stałej szerokości ze znakiem (znak zawsze
  widoczny, wartość wyrównana do prawej), więc cyfry i jednostki stoją w
  miejscu zamiast skakać w bok przy przejściu przez zero lub zmianie liczby
  cyfr.
### Naprawione
- **LOCK mógł tracić lock przy dryfującym OCXO — dostaje teraz łagodny człon
  częstotliwości.** W normalnej gałęzi LOCK ścieżka częstotliwości była
  wyłączona (freq_term = 0), więc jedyną obroną przed realnym dryfem OCXO był
  wolny feed-forward dryfu. Na ciepłym sprzęcie mocno się opóźniał, a faza
  wychodziła z okna locka (11 → −425 ns w 51 s, potem LOCK→DPLL→ACQ). LOCK
  stosuje teraz lekki człon 0,1×Kp — dość, by anulować bieżący dryf w każdym
  kroku, na tyle łagodny, by nie wtrącać szumu TIM2 do cichego locka. Łączy się
  z szybszym feed-forwardem (niżej). Analiza przyczyny: GML-5.2.
- **Usunięty limit-cycle w ACQ (bujanie PWM ±150 LSB).** Algorytm 10 brał błąd
  częstotliwości z avg10 (kwantyzacja 0,1 Hz); razy Kp (~1550 LSB/Hz) dawało to
  skoki PWM ±150 LSB w cyklu ~10 s, które nie pozwalały fazie ustabilizować się
  poniżej progu locka i spowalniały akwizycję. Teraz używa avg100 (0,01 Hz),
  10× drobniejsze, i akwizycja ustala się czysto. Analiza: GML-5.2.
- **Feed-forward dryfu robi teraz bootstrap po locku.** Jego pierwsze okno
  uczenia było wolne (30 s), więc szybki dryf po locku uciekał zanim ruszył.
  Teraz robi trzy szybkie okna 8 s z większym krokiem tuż po locku (absorbując
  dryf w ~10–20 s), potem wraca do cichego reżimu 30 s.
- **Dolna klamra tłumienia podniesiona 0,30 → 0,45.** Learner mógł tłumić tak
  mocno, że pętla miała tylko 30% mocy korekcji i nie nadążała za dryfem; 0,45
  wciąż tłumi cykl graniczny, ale zachowuje dość mocy, by śledzić.

### Zmienione
- **Kotwica kalibracji LC jest teraz uniwersalna — 0,632·Vsat, wyliczana per
  płytka.** Detektor to rampa ładowania RC V(φ) = Vsat·(1 − e^(−φ/τ)); punkt
  φ = τ, gdzie V = 0,632·Vsat, to ta sama względna wysokość na każdym detektorze
  wykładniczym, niezależnie od Vsat. LC odzyskuje teraz Vsat dopasowaniem 1-D
  (linearyzacja −ln(1 − V/Vsat) względem t, wybór Vsat o najmniejszej reszcie)
  i kotwiczy tam. Poprzednie zaszyte 1,85 V działało tylko dlatego, że detektory
  Marka i Dana Wiering mają Vsat ≈ 2,93 V; detektor o innym Vsat minąłby pasmo.
  LC samoadaptuje się teraz per płytka bez konfiguracji, a `LTIC_ZERO_ANCHOR_V`
  zostaje wycofane. Zweryfikowane na zalogowanych przebiegach: Vsat odzyskane do
  ~0,3%, kotwice zgodne ~0,8% między przebiegami. Fizyka i wyprowadzenie: GML-5.2.
- **Podtytuł splasha** brzmi teraz `GPS Disciplined OCXO` (spacja, nie myślnik).

### Podziękowania
- Diagnoza anomalii pętli i wyprowadzenie uniwersalnej kotwicy w tym wydaniu
  pochodzą od **GML-5.2**, zweryfikowane tutaj względem zalogowanych danych i
  zachowania sprzętu. Logi polowe i testy: **danieljw** (wzorzec Rb) i **lucido**.

---

## [v0.91-rtos] — 2026-07-11

### Dodane
- **Kalibracja LC — zakotwiczony punkt pracy + lokalne nachylenie ns/V (Opcja D).**
  Rampowy detektor fazy jest wykładniczy (1k/1n, τ≈1 µs), więc ns/V nie jest
  stałe wzdłuż rampy, a średnia po całym przejściu (range/span) rozjeżdżała się
  o ~15–20 % między uruchomieniami — zależnie od tego, gdzie arm picDIV zaparkował
  fazę. ns/V liczone jest teraz z LOKALNEGO nachylenia dV/dt w oknie ±0.20 V wokół
  stałego punktu pracy (LTIC_ZERO_ANCHOR_V = 1.85 V). zero_offset jest
  zakotwiczone w tym punkcie — powtarzalnym środku rampy, z dala od stref martwych
  detektora zmierzonych przez Dana Wiering (spadek na diodzie Schottky + pull-down
  poniżej ~0.05 V oraz rail/zawinięcie ADC przy ~3.3 V). Jeśli przebieg nigdy nie
  przekroczy pasma kotwicy, kod wraca do dawnej średniej range/span i to sygnalizuje.

  Ustalenia z kilku logów LC o rozdzielczości 1 s:
  * Kotwica jest dokładna — kolejne przebiegi za każdym razem lądują z
    zero_offset = 1,8500 V.
  * Rozrzut ns/V między przebiegami spadł z ~15–20 % (dawna średnia range/span)
    do kilku procent. Przy obu przebiegach z TYM SAMYM rate wynosi ~2,8 %;
    resztę zdominowała kwantyzacja rate przemiatania, nie dopasowanie nachylenia —
    avg100 rozróżnia rate z ziarnem 1 ns/s, więc etykieta „−5” vs „−6” niesie
    ±0,5 ns/s i pasma ufności obu ns/V się przekrywają. Nie szkodzi to LOCK-owi:
    pętla używa dokładnie tego ns/V, które zmierzyła, w napięciu, w którym
    faktycznie pracuje.
  * Okno dopasowania poszerzono do ±0,20 V (LTIC_ANCHOR_WIN_V): więcej punktów w
    paśmie (~70 vs ~35) uśrednia szum ADC, redukując rozrzut przy tym samym rate
    z ~5,9 % przy ±0,10 V do ~2,8 %.
- **Diagnostyka LC co sekundę.** Podczas przebiegu próbkującego LC drukuje teraz
  jedną linię `t=/V=/n=` na sekundę, uwidaczniając całą rampę w logu (posłużyło do
  wyprowadzenia Opcji D).

### Naprawione
- **Raport szeregowy drukowany dwa razy na sekundę w RD/RH przy aktywnym fiksie GPS.**
  vDisplayTask jest budzony przez dwa źródła ~1 Hz — tor częstotliwości (co PPS)
  i parser GPS (co zdanie czasu) — więc przy fiksie budził się dwa razy na sekundę
  i emitował dwie linie raportu. Linia szeregowa jest teraz bramkowana zmianą
  licznika PPS, więc drukuje się dokładnie jedna na sekundę; wyświetlacz nadal
  odświeża się przy każdej notyfikacji. Zgłoszone przez Dana Wiering.
- **Pisownia nazwiska w podziękowaniach** poprawiona na „Wiering” (na prośbę autora).
- **Odczyt fazy `Vph` na TFT był martwym kodem, a przy włączeniu — błędnym.**
  Warunkował się stałą kompilacyjną `LTIC_NS_PER_VOLT` (domyślnie 0, więc wartość
  ns nigdy się nie pokazywała po kalibracji), a gdyby stała była ustawiona,
  liczył `V × ns_per_volt` od 0 V zamiast względem `zero_offset`. Teraz używa
  ZMIERZONYCH `g_ltic.ns_per_volt` i `zero_offset` z LC, pokazując fazę ze znakiem
  `(V − zero_offset) × ns/V` zgodną z błędem pętli, albo same wolty gdy
  nieskalibrowane.
- **CT odrzucał wąskie (lepsze) OCXO.** Sanity check wzmocnienia obiektu miał
  dolny próg K = 0,1 mHz/LSB, ale wąski span EFC jest pożądany — mniejsze Hz/LSB
  to lepsza rozdzielczość i jedna z dróg do E-12. Sprzęt Dana Wiering mierzy
  0,048 mHz/LSB (~1,05 V span EFC) i był błędnie odrzucany. Próg obniżony do
  0,02 mHz/LSB; odrzucane są teraz tylko przebiegi szum/brak-GPS.
- **Poprawki układu ILI9488 (480×320) — ze zdjęć użytkowników, jeszcze nie
  zweryfikowane na panelu.** Pierwsi użytkownicy Dan Wiering i lucido przysłali
  zdjęcia swoich buildów 480×320. Kilka problemów rozwiązano na ich podstawie
  bez panelu pod ręką: (1) font głównych danych był nadmiernie skalowany —
  TFT_F mapował font 2→4 (rosnąc 1.63× gdy wiersze skalują się tylko 1.33×),
  więc linie nachodziły w pionie, a pasek statusu wypadał poza ekran; font
  danych zostaje teraz na 2. (2) Wiersz czujnika BMP skrócono (temperatura i
  ciśnienie do 1 miejsca), by szersze skalowane glify nie nachodziły na kolumnę
  AHT. (3) W instrukcji `User_Setup.h` brakowało `LOAD_FONT8`, którego wymaga
  odczyt częstotliwości — bez niego ta linia zostaje pusta. To poprawki „na
  najlepsze wyczucie” ze zdjęć; finalne przejście po geometrii nastąpi, gdy
  będzie dostępny panel ILI9488. Małe panele 320×240 są nietknięte (TFT_F jest
  tam tożsamością).
- **LOCK mógł tracić lock przy dryfującym OCXO (odbicie LOCK→DPLL→ACQ).** Przy
  realnym dryfie częstotliwości (zmierzone ~8,5 ns/s na ciepłym sprzęcie) faza
  wychodziła z okna locka — 11 → −425 ns w 51 s — a korekcja była za słaba, by
  nadążyć: learner tłumienia dobił do 0,30 (korekcja na 30% mocy), a
  feed-forward dryfu wciąż zbierał pierwsze okno 30 s, więc nie ruszył zanim
  lock został utracony. Dwie zmiany: dolna klamra tłumienia podniesiona 0,30 →
  0,45 (zachowuje dość mocy, by śledzić dryf, wciąż tłumiąc cykl graniczny), a
  feed-forward robi teraz BOOTSTRAP po locku — trzy szybkie okna 8 s z większym
  krokiem absorbują dryf w ~10–20 s, potem wraca do wolnego, cichego reżimu
  30 s. W symulacji na zalogowanym dryfie faza trzyma się teraz ~−125 ns zamiast
  uciekać. Stabilne setupy o małym dryfie (np. build z referencją Rb) są
  nietknięte — bootstrap zbiega natychmiast, a wyższa klamra to nadal netto
  tłumienie.
- **Faza w ns dodana do raportu szeregowego**, po `Vphase:`, gdy LC skalibruje
  detektor — `(V − zero_offset) × ns/V`, ta sama konwencja co pętla i wiersz TFT.
- **Kotwica LC to teraz zmierzony środek rampy, nie stałe 1,85 V.** Kotwica
  lokalnego nachylenia była zaszyta pod pasmo detektora Marka; sprzęt, którego
  rampa przemiata inny zakres (Dan pracuje niżej, ~1,3 V), całkiem mijał okno
  kotwicy i wracał do zgrubnej średniej range/span (wynik „weak”). Kotwica to
  teraz `vlow + span/2` z rzeczywistego przebiegu, a stała `LTIC_ZERO_ANCHOR_V`
  jest używana tylko gdy faktycznie mieści się w przemiecionym paśmie. LC
  samoadaptuje się per płytka.
- **Ciśnienie na TFT mogło wchodzić na kolumnę AHT.** Ciśnienie BMP drukowane było
  z 2 miejscami (`1013.25hPa`), co przy 4-cyfrowym ciśnieniu wychodziło poza lewą
  kolumnę. Zmniejszone do 1 miejsca (`1013.2hPa`), spójnie z raportem szeregowym.
- **Odbicie LOCK przy ciepłym starcie (marnowało ~1 min z ~8 min boot-to-lock).**
  Zapisany LOCK/DPLL był wznawiany, dopóki odczyt fazy był ważny (na rampie),
  nawet gdy siedział daleko od zero_offset — np. Vphase ≈2,09 V przy kotwicy
  1,85 V (~260 ns od centrum). LOCK wtedy wchodził, DPLL po minucie uznawał fazę
  za zbyt odległą i schodził aż do ACQ, więc pełna akwizycja i tak się wykonywała
  po zbędnym objeździe. Guard startowy demotuje teraz zapisany LOCK/DPLL do ACQ,
  chyba że faza jest ważna I mieści się w oknie ACQ wokół zero_offset. Zimny start
  bez zmian (stan domyślnie ACQ); naprawdę wycentrowany ciepły start nadal wznawia
  LOCK natychmiast.

---

## [v0.90-rtos]

### Dodane
- **Bufor pierścieniowy w Flashu z równoważeniem zużycia dla danych “żywych”.**
  Nauczony dryf/tłumienie, kalibracja LC i ostatni PWM są teraz auto-zapisywane
  do dedykowanego sektora Flasha (sektor 6, 0x08040000, 128 KB) jako pierścień
  32-bajtowych slotów. Każdy zapis programuje kolejny pusty slot; sektor jest
  kasowany dopiero przy zawinięciu pierścienia (raz na 4095 zapisów), więc przy
  100 zapisach/dobę Flash starczy na rzędu tysiąca lat. Każdy slot ma CRC i numer
  sekwencji; slot zapisany połowicznie (zanik zasilania) nie przechodzi CRC i
  używany jest poprzedni dobry. Nagłówek z sygnaturą i wersją formatu czyni
  firmware odpornym na pełne kasowanie układu, programowanie sektorowe, pierwszy
  start i śmieci we Flashu (obcy lub pusty sektor jest wykrywany i re-inicjowany).
- **Auto-zapis z histerezą.** Dane żywe zapisywane tylko gdy ustabilizują się na
  nowej wartości: dryf zmienił się o > 8 LSB lub tłumienie o > 0.03, ORAZ minęło
  co najmniej 20 min od ostatniego zapisu. Udana kalibracja `LC` zapisuje od razu.
- **Komenda `FR 0|1`** (zapis `ES`, domyślnie włączone) przełącza bufor w czasie
  pracy — bez flagi kompilacji, więc bez niespodzianek z cache buildu. `FR 0`
  zatrzymuje całą aktywność bufora.
- **Komenda `EW`** pokazuje diagnostykę zużycia Flasha: cykle kasowania i użyte sloty.
- **Korekcja piły (qErr) dla LTIC (`SAW 0|1`).** Odbiorniki czasowe u-blox
  generują 1PPS przez dzielenie wewnętrznego zegara, więc każdy impuls pada do
  jednego okresu zegara obok prawdziwego czasu GPS — to błąd kwantyzacji per
  impuls, który odbiornik raportuje jako `qErr` w UBX-TIM-TP. Pasywny sniffer
  parsuje ten komunikat (qErr to 32-bitowe pole pikosekundowe na tym samym
  offsecie w LEA-6T, LEA/NEO-M8T i ZED-F9T, więc jeden parser obsługuje
  wszystkie), a tor fazy TIC go odejmuje, usuwając piłę granularności odbiornika
  i zostawiając własny błąd OCXO. Na LEA-6T (granularność 21 ns) to dominujący
  krótkookresowy składnik fazy. TIM-TP włączane automatycznie przy inicjalizacji
  GPS; `SAW` przełącza korekcję (zapis przez `ES`, domyślnie wyłączona) i
  pokazuje qErr na żywo.

### Zmienione
- **`ES` nie nadpisuje już nauczonych/skalibrowanych wartości gdy bufor jest włączony.**
  Przy `FR 1` kalibracja (ns_per_volt, zero_offset, range_ns, centre_v) oraz
  nauczony dryf/tłumienie należą wyłącznie do bufora; `ES` zapisuje tylko
  prawdziwe ustawienia (nastawy PID, progi, flagi). Przy `FR 0` `ES` nadal
  zapisuje te wartości żywe do EEPROM jako fallback, a `eeprom_recall()` ładuje
  je przy starcie, więc migracja starszego EEPROM zachowuje kalibrację.

### Naprawione
- **`LA 10`: brzydki rozruch — persystowany LOCK przy nasyceniu.** Po restarcie
  alg 10 czytał `g_ltic.state=LOCK` z EEPROM i startował prosto w LOCK, ale OCXO
  zdążył termicznie dryfnąć, więc detektor startował nasycony (Vphase 3,09 V).
  Saturation guard oznaczał fazę invalid → hold PWM → utknięcie w nasyceniu ~6 min
  aż DPLL→ACQ w końcu przejął. Naprawa: na PIERWSZYM wywołaniu po boocie
  (`prev_state == 0xFF`), jeśli persystowany stan to LOCK/DPLL ale faza invalid,
  demote do ACQ — pełny pull-in + re-arm picDIV od razu.
- **`LA 10`: limit-cycle ~370 s w LOCK — nasycenie detektora + windup
  integratora.** Vphase w LOCK oscylował 0,024↔0,963 V (≈pełen zakres detektora,
  23 % próbek blisko nasycenia), okres ~370–550 s — pętla sama generowała cykl,
  nie szum OCXO. Root cause: odczyt z nasycanego detektora dawał fałszywą fazę
  ~1000 ns (bo `ltic_phase_error_ns` akceptował V < 3,28 jako valid), którą
  integrator całkował → overshoot → ponowne nasycenie. Naprawa: `ltic_phase_error_ns`
  oznacza teraz odczyty poza kalibrowanym pasmem liniowym (±55 % span wokół
  zero_offset) jako invalid, a DPLL/LOCK dla `!ph_valid` freeze PHASE path
  (proporcja + całka) i zostawia tylko ścieżkę FREQUENCY (TIM2 widzi prawdziwy
  offset niezależnie od nasycenia detektora). TIM2 wciąga OCXO z powrotem w okno,
  faza staje się valid i PI wznawia.
- **`LA 10` self-learn: damp utknął na 0,5 + drift łowił cykl.** Obserwator
  limit-cycle miał stały próg amplitudy 5 ns — dla detektora HC74 (zakres 1650 ns,
  szum ADC ~50 ns) każdy cykl przekraczał próg, więc damping zawsze dekrementował
  aż do podłogi `LRN_DAMP_LO=0,5`, a feed-forward gonił oscylację zamiast ją
  gasić. Naprawa: próg skalowany do zmierzonego zakresu detektora (3 % range,
  clamp 5..150 ns), próg sign-crossing też skalowany, krok dekrementacji
  ograniczony (0,10 max), a `LRN_DAMP_LO` obniżony do 0,30. Teraz cicha pętla
  relaksuje damping z powrotem w górę, głośna — tłumi mocniej.
- **`LC` nie walczy już z pętlą dyscypliny.** Uruchomienie `LC`, gdy algorytm
  10 aktywnie dyscyplinował, pozwalało pętli ruszać PWM jednocześnie ze
  sweepem kalibracji, więc oba się nawzajem psuły — zmierzone tempo sweepu
  wychodziło ±1 ns/s, a zakres jako absurdalne wartości (1502 / 3518 ns),
  które bramka fizyki słusznie odrzucała. Pętla sterująca jest teraz wyciszana
  gdy trwa kalibracja (`g_calib_active`), więc `LC` można uruchomić w dowolnej
  chwili, także pod `LA 10`.
- **Ścieżki PWM bezpieczne podczas kalibracji.** Ten sam guard obejmuje teraz
  także sterowanie holdoverem termicznym algorytmu 9 oraz ręczne komendy PWM
  (`up1`/`up10`/`dp1`/`dp10`/`SP`), które są odrzucane z jasnym komunikatem gdy
  trwa `LC`/`CT`, więc żadna ścieżka nie zaburzy trwającego sweepu.
- **Brak zawinięcia w `LC` nie jest już traktowany jako porażka.** Detektor,
  który nie zawija w oknie sweepu, przechodzi teraz z dobrym slope/centre/span
  i jest auto-zapisywany; tylko naprawdę słaby wynik (mały span lub centre poza
  pasmem) jest sygnalizowany, z konkretnym powodem. Komunikaty nie każą już
  użytkownikowi robić `ES` po `LC` — udane `LC` auto-zapisuje do flash ringa
  (to dane żywe). `CT` nadal prosi o `ES`, bo stroi nastawy PID.

### Kredyty
- Doprecyzowano atrybucję: André Balsa jako autor v0.06c, inspiracji portu RTOS.
  Poprawiono link do repozytorium.

---

## [v0.89-rtos]

### Dodane
- **Samouczący się układ pomocniczy pętli (`LRN`), wspólny dla algorytmu 7 i
  LTIC.** Dwa wolne, pasywne uczniowie — na podstawie nocnych śladów Dana
  Wiering (wzorzec Rb): piła fazy ~9000 s ±80 ns, garb ADEV przy stałej
  czasowej pętli, dryf 8E-12/dobę: (1) **feed-forward dryfu** — estymuje
  średnie nachylenie fazy OCXO w oknach 30 s i dodaje człon PWM kasujący je,
  więc pętla przestaje gonić ruchomy cel, a faza się spłaszcza; (2)
  **adaptacja tłumienia** — obserwuje przejścia błędu fazy przez zero i
  obniża wzmocnienie przy przeregulowaniu, podnosi przy ospałości — zbijając
  garb ADEV przy stałej czasowej. Oba działają TYLKO w LOCK, aktualizują się
  co najwyżej raz na 30 s i są twardo ograniczone (feed-forward ±400 LSB,
  tłumienie 0,5–1,5), więc zła estymacja nie rozstroi pętli; żaden nie wtrąca
  pobudzenia. `LRN 1|0` włącza/wyłącza (domyślnie wł.), `LRN R` resetuje do
  teorii, samo `LRN` wypisuje stan; wartości zapisuje `ES` (EEPROM 222–230) i
  odtwarza przy starcie. Raport szeregowy pokazuje żywy wiersz `Learn:` (dryf,
  nachylenie, tłumienie, zaobserwowany okres/amplituda cyklu granicznego).
- **Uczenie obejmuje teraz każdy algorytm dyscypliny (3–10), nie tylko 7/8.**
  Jeden wrapper `lrn_apply()` podaje uczniom własny akumulator fazy i błąd
  częstotliwości każdej pętli; sieć NN (algo 9), nie mając jawnego
  akumulatora fazy, używa tylko tłumienia. Stan `LRN` jest wspólny dla
  wszystkich algorytmów.

### Interfejs / Wyświetlacz
- **Kolorowy TFT przerobiony dla czytelności i odrobiny życia.** Spójne
  formatowanie etykiet z pojedynczą spacją (`Alt: 144m`, `PWM:...`,
  `Uptime: ...`); wartości wyrównane optycznie w foncie proporcjonalnym.
  Granatowa ramka (jak nagłówek) obejmuje obszar danych, trzy separatory
  spięte bocznymi liniami. Częstotliwość zielenieje przy locku. Dodana
  etykieta `DATE:`.
- **Splash powitalny dopieszczony**: tytuł na wysokości częstotliwości, dwie
  fale oscylatorów wyłaniające się z przesunięciem fazy, schodzące się i
  zlewające w jedną zieloną falę z narastającą i zanikającą poświatą,
  a następnie przewijana lista detekcji sprzętu (okno stałej wysokości,
  kredyty nieruchome).
- **Komenda `SPL 0|1`** (zapis `ES`, domyślnie 1) przełącza animację
  powitalną. `SPL 0` pokazuje sam tytuł i kredyty przez dwie sekundy — dla
  obojętnych na sztukę.

---

## [v0.88-rtos]

### Naprawione
- **Pole częstotliwości TFT nie zostawia już fragmentów cyfr po
  komunikatach CAL/WARMUP/SVIN.** Komunikaty i duża częstotliwość mają
  różne wysokości tekstu, więc padding czyścił tylko pas bieżącego
  fontu; całe pole jest teraz czyszczone przy każdej zmianie trybu.

### Usunięte
- **Usunięto obsługę mostka SPI→T6963C** (eksperyment): `T6963C_Bridge.h`,
  jego sekcja w tasku wyświetlaczy, blok konfiguracyjny i odwołania.

### Dokumentacja
- README (EN/PL/ES) zaktualizowane o funkcje LTIC v0.5x–v0.88 (auto-
  kalibracja LC, automatyczne wzmocnienia, medianowy tor ADC, strażnik
  ucieczki, WU, animacje LED, wiarygodny kolor locka) oraz nową sekcję o
  obsłudze kolorowych TFT: dowolny panel TFT_eSPI 320×240 lub 480×320
  z opisem podłączenia.

---

## [v0.87-rtos]

### Naprawione
- **Zero martwego czasu przed próbkowaniem — przygotowania zjadały całe
  pasmo.** ADC nadąża spokojnie (1 próbka/s ≈ 8 mV/krok przy 9 ns/s); zawiodło
  ~60 s stabilizacji i pomiarów d1/d2 między zadaniem rampy a pierwszą
  próbką. Stały offset nakłada się na df, które zapisany PWM już ma
  (zmierzone +9 ns/s na żywo), więc faza przeleciała 0,061→2,62 V przez całe
  pasmo ZANIM próbkowanie ruszyło, a fit widział samo nasycenie. LC teraz
  re-armuje picDIV (deterministyczny start od dołu), zadaje offset i zaczyna
  próbkować w ~3 s; dokładne tempo czytane jest PO przebiegu z czystego
  avg100. Jeśli nasycenie przyjdzie przed 10 punktami fitu, offset jest
  połowiony, picDIV re-armowany i przebieg powtórzony raz. Przedprzemiotowy
  pomiar d1/d2 i maszyneria adaptacyjna reduce/increase zostały usunięte —
  bramka fizyki i precyzyjne tempo po przebiegu czynią je zbędnymi.

---

## [v0.86-rtos]

### Zmienione
- **LC przeprojektowany jako pojedynczy przebieg dół→góra — bez sondowania
  kierunku, bez flipów, bez potrzeby zawinięć.** Logi z anteny dowiodły, że
  uzbrojenie picDIV parkuje fazę DETERMINISTYCZNIE ~60 ns nad punktem
  synchronizacji (Vphase ≈0,061 V po każdym re-armie), że strona ujemna
  poniżej tego punktu jest MARTWA (kolejność zboczy się odwraca, impuls
  znika — avg100 pokazywało realny dryf −3 ns/s przy stojącym napięciu),
  a strona dodatnia prowadzi całe pasmo w miękkie nasycenie. LC teraz to
  wykorzystuje: po uzbrojeniu ZADAJE dodatni przemiot ~+4 ns/s (offset ze
  zmierzonego K), próbkuje całe pasmo w jednym przebiegu, a utrzymane górne
  nasycenie traktuje jako naturalny KONIEC pomiaru, nie usterkę. Precyzyjny
  odczyt avg100 (v0.85) dokładnie skaluje ns/V. Flip kierunku w przemiocie
  i jego maszyneria restartu zostały usunięte.

---

## [v0.85-rtos]

### Naprawione
- **Odwrócenie kierunku ZADAJE teraz tempo przemiotu zamiast ufać ślepemu
  odczytowi — i faza nie parkuje już przy krawędzi pasma.** Na żywo iteracja
  flip zatrzymała się na nominalnym „−1 ns/s", które realnie było ≈0: avg10
  kwantuje po 0,1 Hz (d1=0.1000, d2=0.0000 w logu), więc poniżej 0,1 Hz odczyt
  to szum. Przy df≈0 faza siedziała tam, gdzie zostawił ją re-arm picDIV
  (Vphase 0,061 V — dolna krawędź pasma, gdzie zbyt wąskie impulsy ledwo
  ładują RC), przemiot pokrył 5 mV, a bramka fizyki musiała przerwać. Teraz,
  gdy znak odwraca się między iteracjami, LC interpoluje punkt 10 MHz P0 z
  dwóch ostatnich offsetów i ustawia rampę na P0 − 0,06 Hz·(LSB/Hz) — ZADANE
  −6 ns/s wyprowadzone ze zmierzonego K, niezależne od skwantowanego odczytu.
  Na końcu przemiotu (PWM stały przez cały czas, więc avg100 jest czyste z
  rozdzielczością 0,01 Hz) prawdziwe tempo jest odczytywane i zastępuje
  zadane przed liczeniem ns/V, więc skala dopasowania jest dokładna.

---

## [v0.84-rtos]

### Naprawione
- **Odwrócenie kierunku w przemiocie prze-mierza teraz tempo i WYMUSZA zmianę
  znaku.** Obrony v0.83 zadziałały na żywo poprawnie (miękkie nasycenie → flip
  → czysty restart → zły wynik odrzucony), ale sam flip miał dwa defekty:
  (1) ns/V z dopasowania dzieli przez phase_rate, a po flipie używane było
  tempo sprzed flipu — gwarantowana zła skala (ns/V=9,09e6 odrzucone przez
  strażnika); (2) lustrzane odbicie offsetu wokół saved_pwm nie zmienia znaku
  dryfu, gdy saved_pwm leży daleko od prawdziwego punktu 10 MHz (+70 dawało
  +0,100 Hz, −70 wciąż +0,054 Hz — strona railująca, tylko wolniej). Po flipie
  LC mierzy df od nowa, a jeśli znak się nie odwrócił, dopycha offset o
  −2·df·(LSB/Hz) ze zmierzonego K i sprawdza ponownie (≤3 iteracje); okno
  odrzucania glitchy jest przeskalowywane do nowego tempa. Symulacja na
  dokładnie tych liczbach z anteny: jedno dopchnięcie ląduje na −0,054 Hz
  (−5,4 ns/s) — strona zawijająca, idealne tempo przemiotu.

---

## [v0.83-rtos]

### Naprawione
- **`LC` nie daje się już oszukać miękkiemu nasyceniu RC.** Przebieg z szybkim
  początkowym offsetem (10 ns/s) pozwolił fazie wjechać w rejon miękkiego
  nasycenia RC (2,9-3,27 V — poniżej progu railu 3,28 V, więc „żywe"):
  dopasowanie liniowe połknęło płaskie punkty nasycenia (ns/V ×74 za duże),
  późniejsze zejście z nasycenia („skok" 2,57 V) zostało przyjęte jako
  zawinięcie, a wynik (range=209204 ns, zero_offset=1,34 V — poza pasmem
  detektora) nawet PRZESZEDŁ samospójność wolt-wolt. Trzy bramki względne do
  pasma zamykają tę klasę: (1) **bramka fizyki** — zapisany zakres nie może
  przekraczać tego, co przemiot mógł fizycznie pokryć (~tempo × okno × 1,5),
  inaczej params unchanged; (2) **końce skoku zawinięcia** muszą leżeć w
  czystym paśmie dopasowania ±50%, więc zejście z nasycenia nie jest
  zawinięciem; (3) **pomijanie miękkiego nasycenia** — gdy dopasowanie ma już
  kształt, próbki daleko poza jego pasmem są traktowane jak railowane
  (pomijane; zasilają logikę odwracania kierunku w przemiocie). Wszystkie trzy
  skalują się z obserwacji bieżącego przebiegu — detektory pełnozakresowe
  3,3 V pozostają nietknięte.

### Dodane
- **Animacja survey-in na wyświetlaczach LED.** Spinner górnego 'o' (segmenty
  A→B→G→F krążące po górnym oczku cyfry), z przesunięciem fazy na cyfrę w
  falę — wizualnie odróżnialny od dolnego 'o' fali warmup.

---

## [v0.82-rtos]

### Naprawione
- **ACQ parkował fazę pół zakresu od punktu przekazania — wieczny ACQ (1401
  cykli na żywo przy Δf≈0).** Cel przyciągania ACQ liczony był jako
  `zero_offset + span/2` — relikt sprzed v0.66, gdy zero_offset był dołem
  pasma; od tamtej pory zero_offset JEST środkiem pasma, więc pętla trzymała
  fazę w swoim „środku", a próg ACQ→DPLL (mierzony względem zero_offset) nigdy
  nie mógł być spełniony. Teraz jeden punkt prawdy: ACQ ciągnie dokładnie do
  zero_offset. Świeże `LC` kasuje też stary override `LCV` (który mógł po
  cichu przywrócić ten sam pat z EEPROM).

### Dodane
- **Animacja warmup na wyświetlaczach LED.** Podczas wygrzewania OCXO każda
  cyfra TM1637/HT16K33 pokazuje spinner małej litery 'o' z przesunięciem fazy
  na cyfrę, więc wzór wędruje przez wyświetlacz jak fala (survey-in zachowuje
  kreski).

### Uwaga
- Po aktualizacji uruchom raz `LC`: poprzednia kalibracja powstała na starym,
  10-sekundowo uśrednianym torze ADC i jej zero_offset/range są rozmyte;
  przebudowany tor z medianą paczki (v0.79) daje ostrzejszy pomiar.

---

## [v0.81-rtos]

### Naprawione
- **Naprawa kompilacji:** `p_eff` był używany przez integrator DPLL/LOCK przed
  deklaracją (v0.79/v0.80 nie kompilowały się). Blok deadband/miękkie kolano
  jest teraz liczony najpierw, więc widzą go i integrator, i człon fazowy.
- **Odliczanie kalibracji pokazuje REALNY czas całości.** Licznik restartował
  się dla każdego wewnętrznego segmentu oczekiwania (30 s, 20 s…), więc
  wyświetlacz nigdy nie odzwierciedlał całej procedury. `LC`/`CT` ładują teraz
  realistyczny total, a fazy adaptacyjne (podbicie rampy, rail-backoff,
  odwrócenie kierunku, restart przemiotu) doładowują go w trakcie; każda
  ścieżka wyjścia go zeruje.
- **Warmup OCXO przywrócony i zapisywalny.** Warmup był po cichu pomijany przy
  ważnym EEPROM — „znikał" po zapisaniu konfiguracji, a zimny OCXO był
  dyscyplinowany jeszcze w dryfie termicznym. Teraz warmup działa domyślnie
  przy każdym starcie, a wyłącza go nowa komenda `WU 0` (`WU 1` włącza; stan
  zapisuje `ES` w bajcie 221 EEPROM, świeży flash: włączony).

### Dodane
- **LED „CAL" + animacja podczas każdej kalibracji.** TM1637 i HT16K33
  pokazują CAL na pierwszych trzech cyfrach, a na czwartej animację gonionego
  segmentu (G→C→D→E) kreślącą małą literę 'o' — czytelny sygnał „pracuję".

---

## [v0.80-rtos]

### Naprawione
- **Zielony kolor częstotliwości oznacza teraz wiarygodny, BIEŻĄCY lock.** Po
  wypadnięciu LTIC z LOCK do ACQ wyświetlacz zostawał zielony, bo średnia
  1000 s wciąż pokazywała ~10 MHz — echo przeszłości, nie teraźniejszość.
  Zasady teraz: dla algorytmu 10 zieleń pochodzi WYŁĄCZNIE z żywego stanu LOCK
  pętli (bez fallbacku na średnie); dla algorytmów 0-9 kryterium długiego okna
  zostaje, ale musi być potwierdzone szybką średnią 10 s wciąż w ±50 mHz od
  10 MHz, więc utrata dyscypliny gasi zieleń w ~10 s zamiast w minutach.

---

## [v0.79-rtos]

### Naprawione
- **Przebudowany tor ADC LTIC — 10-sekundowa średnia krocząca zatruwała
  pętlę.** Stary tor brał JEDEN surowy odczyt ADC na PPS przez 10-próbkową
  (=10 s) średnią kroczącą: ~5 s opóźnienia grupowego (pętla korygowała na
  nieświeżych danych), a co gorsza napięcia sprzed i po zawinięciu mieszały się
  w fantomowe poziomy pośrednie — pętla widziała gładki dryf ~30 ns/s, który
  fizycznie nie istniał, i kopała prawdziwą fazę (kroki LOCK do 152 LSB,
  odbijanie LOCK↔DPLL). Teraz każdy slot PPS bierze paczkę 16 odczytów (~1 ms)
  i jej MEDIANĘ — bez pamięci międzysekundowej, bez lagu, bez mieszania przez
  wrap, pojedyncze glitche wypadają — plus bramka outlierów: skok >25%
  skalibrowanego zakresu musi się powtórzyć w następnym odczycie, by być
  uznany (prawdziwe zawinięcia trwają; glitche nie). Uwaga: częstsze
  odczytywanie ADC nic by nie dało — detektor ładuje kondensator raz na PPS,
  więc informacja o fazie jest z natury 1 Hz; paczka maksymalizuje jakość tej
  jednej próbki.
- **LOCK łagodny z założenia: deadband + miękkie kolano + limit kroku.**
  Wewnątrz deadbandu (zakres/40, ≥6 ns — poziom szumu ADC) błąd fazy liczy się
  jako zero, a integrator stoi; poza nim błąd narasta od zera (miękkie kolano);
  końcowy krok LOCK jest twardo ścięty na ≈4 mHz (ze zmierzonego K). Małe
  odchyłki dostają teraz proporcjonalnie małe pchnięcia zamiast kopnięć pełnym
  wzmocnieniem.

---

## [v0.78-rtos]

### Naprawione
- **Pierwszy potwierdzony LOCK na żywo z trójstanową pętlą LTIC.** Dwa
  domknięcia: (1) odczyt częstotliwości na TFT zielenieje teraz przy LTIC LOCK
  — rozpoznawał tylko dawne „hit", więc kolor czekałby aż średnie 1000/10000 s
  dojdą do mHz; (2) strażnik odczytu EEPROM odrzucał algorytm 10
  (`algo > 9 → 0`), więc zapisana konfiguracja LTIC po restarcie po cichu
  wracała do algorytmu 0 — teraz `> 10`. Z tym `ES` w pełni utrwala zestaw
  LTIC: algorytm 10, kalibracja LC i polaryzacja są zapisane, a wzmocnienia
  pętli autotune wylicza na nowo z zapisanych pomiarów przy każdym wejściu,
  więc po restarcie urządzenie wraca gotowe do locka bez żadnych ręcznych
  kroków.

---

## [v0.77-rtos]

### Naprawione
- **Przejścia stanów nie odbijają już na schodkowym odczycie detektora.** Przy
  wreszcie utrzymanej częstotliwości (−0,02 Hz) pętla wciąż ping-pongowała
  ACQ↔DPLL: ADC aktualizuje napięcie fazy schodkami, a każdy schodek dawał
  fantomowe „nachylenie" 50-100 ns/s, które wyzwalało bramki nachylenia z
  napięcia (wejście do DPLL blokowane przez 183 cykle; DPLL degradowany po 6).
  Wszystkie bramki jakości częstotliwości w przejściach używają teraz Δf z TIM2
  (odpornego na schodki) — ACQ→DPLL przy |Δf|≤0,05 Hz, DPLL→LOCK przy ≤0,03 Hz,
  degradacje przy Δf>0,30 / 0,10 Hz — a napięcie służy wyłącznie POZYCJI fazy.
  Degradacja DPLL dostała też tę samą 3-krotną persistencję, którą LOCK już
  miał, więc pojedynczy schodkowy odczyt nie degraduje. Symulacja ze schodkami:
  zero fałszywych degradacji, czysty awans do LOCK.

---

## [v0.76-rtos]

### Dodane
- **Pełne auto-strojenie LTIC — bez ręcznych współczynników.** `ltic_autotune()`
  wyprowadza KAŻDE wzmocnienie pętli z dwóch zmierzonych stałych sprzętu: K
  (Hz/LSB z CT) oraz ns/V + zakres (z LC). Pętla częstotliwości kasuje ~50% Δf
  na krok; pętla fazy ściąga z τ≈20 s; LOCK jest 4× łagodniejszy; próg ACQ staje
  się ćwiartką zmierzonego zakresu detektora. Uruchamia się automatycznie po
  każdej udanej LC i przy wejściu w algorytm 10, wypisując wyliczone wartości.

### Naprawione
- **ACQ gasi teraz błąd częstotliwości z TIM2, nie napięciowy dryf.** Schodkowy
  odczyt detektora płaszczeje przy krawędzi pasma (na żywo: faza zaparkowana na
  0,336 V przy realnym utrzymującym się offsecie −0,3 Hz i odbijaniu ACQ↔DPLL)
  — nachylenie z napięcia jest tam ślepe; TIM2 nie.
- **Polaryzacja płytki nie odwraca już toru częstotliwości.** K jest dodatnie na
  każdej płytce (+PWM → +f), więc człony częstotliwościowe nie przechodzą przez
  `pol`; robi to tylko tor fazowy (Vphase). Prowadzenie e_freq przez pol=−1
  odwracało w DPLL poprawną korekcję częstotliwości — współprzyczyna odbijania
  stanów.

---

## [v0.75-rtos]

### Naprawione
- **ACQ oscylował (wahnięcia ±1 Hz, dwukrotnie mrożony przez strażnika), gdy
  kalibracja stała się wreszcie POPRAWNA.** Wzmocnienie dryfu używało
  zgadniętego stałego mnożnika (×60), niejawnie dostrojonego do starej, źle
  wyskalowanej kalibracji; z prawdziwym ns/V liczbowy dryf urósł ~2,3× i pętla
  przekorygowywała ~1,8× na krok — podręcznikowa oscylacja z przestrzałem.
  Wzmocnienie jest teraz wyprowadzane ze ZMIERZONEJ czułości OCXO (CT zapisuje
  0,40/K w g_pid[7].Kp, więc LSB-na-Hz odzyskuje się jako Kp7/0,40) z
  tłumieniem 0,5: ~60% błędu kasowane na krok, bezwarunkowo stabilne na każdym
  egzemplarzu, bez strojenia pod płytkę. Człon częstotliwościowy DPLL (stałe
  ×1000, ~6× za słaby na tym egzemplarzu) jest skalowany ze zmierzonego K tak
  samo.

---

## [v0.74-rtos]

### Naprawione
- **Bramka jakości skoku zawinięcia — zamyka ostatnią znaną drogę, którą LC
  mogło pójść źle.** Schodkowy ADC potrafi zaraportować zawinięcie w pół kroku,
  dając CZĘŚCIOWY skok; jeden taki został przyjęty jako pełny span (0,122 V na
  detektorze ~0,33 V), co posadziło zero_offset przy dnie (0,09 V) i wysłało
  pętlę w pogoń za fałszywym środkiem, aż częstotliwość uciekła o 3 Hz. Skok
  liczy się teraz tylko, jeśli zaczyna się od żywej (nie-railowanej) próbki
  ORAZ wynosi ≥80% faktycznie zaobserwowanego pasma min–max; częściowe skoki są
  nazwane w logu, a zamiast nich używane jest obserwowane pasmo (lub cross-check
  czasowy). `zero_offset` jest teraz ZAWSZE środkiem obserwowanego pasma, nigdy
  nie pochodzi z pozycji skoku.
- **Linia werdyktu dla operatora.** LC kończy się jawnym „PASSED checks —
  review LL, then 'ES'" albo „MARGINAL result — prefer re-running LC before
  'ES'", więc słabą kalibrację trudno zapisać przez przypadek.

---

## [v0.73-rtos]

### Naprawione
- **Strażnik ucieczki przebudowany po realnej ucieczce 3 Hz do PWM 63500 —
  stary miał trzy fałszywe założenia.** (1) Jego baza zakotwiczała się na nowo
  przy każdej nie-railującej próbce, ale podczas ucieczki faza okresowo się
  ZAWIJA (chwilowo nie-railed), więc baza goniła ucieczkę i próg 6000 LSB nigdy
  nie zadziałał. Teraz baza przesuwa się tylko przy faktycznie zdrowej pętli
  (nie-railed ORAZ |Δf| < 0,25 Hz). (2) Próg w LSB milcząco zakłada czułość
  Hz/LSB danego OCXO; głównym kryterium jest teraz sam zmierzony błąd
  częstotliwości: faza railed ORAZ |Δf| > 0,5 Hz → freeze (zapasowy próg
  2000 LSB zostaje). (3) Zamrożenie kroku zostawiało nakręcający się integrator
  DPLL/LOCK, gotowy walnąć PWM po odzyskaniu — podczas freeze jest re-seedowany
  do trzymanego PWM. Test behawioralny: stary strażnik pozwolił symulowanej
  ucieczce dojść do 6,15 Hz; nowy zamraża przy 0,51 Hz.

---

## [v0.72-rtos]

### Naprawione
- **Odwrócenie kierunku następuje teraz W TRAKCIE przemiotu, tam gdzie rail
  faktycznie się ujawnia.** 8-sekundowa sonda z v0.71 nie mogła złapać złego
  kierunku: w stronę railującą faza wychodzi z okna synchronizacji dopiero po
  ~pełnym zakresie dryfu — kilkadziesiąt sekund w głąb przemiotu (sonda
  przeszła, potem 137 próbek railowało). LC liczy teraz kolejne railowane
  próbki podczas samego przemiotu; utrzymana seria (≥15 s) jest werdyktem
  kierunku: odwraca znak offsetu (lustrzanie wokół zapisanego PWM), ponownie
  uzbraja picDIV, zeruje wszystkie akumulatory i restartuje przemiot raz.
  Zweryfikowane symulacją: zła strona railuje po 40 s → flip po ~54 s → czysty
  przemiot z dobrej strony z uchwyconym skokiem pełnego zakresu. Jeśli railują
  oba kierunki, istniejący abort mostly-railed nadal to zgłosi.

---

## [v0.71-rtos]

### Naprawione
- **`LC` auto-wykrywa KIERUNEK rampy — pierwotną przyczynę każdej railującej
  kalibracji.** Porównanie wszystkich przebiegów ujawniło wzorzec: każda
  nieudana kalibracja miała dodatnie df (rampa wypychała częstotliwość powyżej
  10 MHz), a jedyna czysta (range=318) miała df ujemne. W tej rodzinie
  detektorów faza zawija się piłokształtnie tylko przy dryfie w jedną stronę; w
  drugą impuls po prostu się poszerza, aż RC przyklei się do railu 3,3 V na
  stałe. Dobry kierunek zależy od płytki, więc LC teraz go sonduje: po
  ustabilizowaniu obserwuje fazę ~8 s i jeśli jest przyklejona do railu,
  odwraca znak offsetu, ponownie uzbraja picDIV i stabilizuje ponownie
  (przerywając czysto tylko, gdy railują OBA kierunki). Adaptacyjna rampa
  zachowuje wykryty kierunek. Zweryfikowano też: algorytm 7 NIE działa podczas
  LC (kalibracja blokuje control task), więc interferencja pętli jest
  wykluczona.

---

## [v0.70-rtos]

### Zmienione
- **`LC` jest w pełni samowystarczalny: ignoruje poprzednią kalibrację.** Zgodnie
  z dobrą zasadą operatorską — kalibrujesz ponownie właśnie dlatego, że zapisane
  wartości mogą być błędne — LC nie dziedziczy już niczego z EEPROM/g_ltic: cel
  rampy, próg zawinięcia, okno glitchy i kryterium prep startują z neutralnych
  założeń, a wszystko jest mierzone od nowa. To kończy kaskadę zatruwania, w
  której jedna zła kalibracja (range=6035) źle sterowała trzema kolejnymi.
- **Pomiar zakresu z pojedynczego zawinięcia.** SKOK napięcia przy zawinięciu
  (szczyt piły → dół w jednej próbce) JEST pełnym spanem detektora, więc jedno
  zawinięcie wystarcza: range = |skok| × ns/V. Cel rampy spada do jednego
  zawinięcia w oknie, czyli znacznie łagodniejszy przemiot, który nie wypycha
  już fazy poza okno synchronizacji picDIV na rail (awaria widziana przy
  9-22 ns/s). Dwa zawinięcia, gdy zdarzą się naturalnie, nadal umożliwiają
  niezależny cross-check czasowy.
- **Kryterium prep jest uniwersalne:** czeka na ważną, nie-railującą, stabilną
  fazę — bez zakładanego napięcia środka (pasma detektorów zasadnie różnią się
  między konstrukcjami).

---

## [v0.69-rtos]

### Naprawione
- **Adaptacyjna rampa `LC` jest teraz sprzętowo-agnostyczna i samoograniczająca.**
  Log v0.68 pokazał kaskadę: zatruta poprzednia kalibracja (range_ns=6035 z
  dopasowania na szumie) ustawiła absurdalny cel tempa rampy, adaptacyjne
  zwiększanie go goniło (offset do 1120, 15 ns/s), a szybka rampa wypchnęła fazę
  całkiem poza okno synchronizacji picDIV — impuls detektora zrobił się szeroki
  i napięcie przykleiło się do railu na cały pomiar („180 railed samples").
  Trzy sprzętowo-agnostyczne obrony (nie zakłada się żadnego pasma detektora;
  różne konstrukcje mają od ~0,3 V do pełnych 3,3 V): (1) zapamiętany zakres
  tylko *kieruje* celem rampy przez szeroki anty-śmieciowy clamp (20..5000 ns);
  (2) **rail-backoff** — po każdym zwiększeniu rampy LC obserwuje ~8 s i jeśli
  faza przykleja się do railu, cofa offset o połowę, ponownie uzbraja picDIV dla
  odzyskania synchronizacji i kontynuuje z tempem, na jakie pozwala sprzęt;
  (3) **bramka samospójności** — wyniki są zapisywane tylko, jeśli zakres ÷
  nachylenie implikuje fizycznie możliwą rozpiętość napięcia (≤3,3 V), inaczej
  poprzednia kalibracja zostaje nietknięta (zła LC nie zatruje już następnej).

---

## [v0.68-rtos]

### Naprawione
- **`LC` nie produkuje już bzdur, gdy rampa trafi blisko punktu 10 MHz OCXO.**
  Offset +70 LSB może ledwo rozstroić OCXO (df=0,01 Hz → 1 ns/s), więc w oknie
  nie mogło być prawdziwego zawinięcia — a mimo to skoki odczytu (napięcie fazy
  aktualizuje się schodkowo) przekraczały próg zawinięcia i dawały fałszywe
  „2 wraps", dopasowanie na samym szumie i absurdalne wyniki
  (ns_per_volt=38615, range_ns=6035). Dodano trzy obrony: (1) **adaptacyjne
  zwiększanie rampy** — jeśli dryf jest za wolny na dwa zawinięcia w oknie,
  offset jest podwajany (limit ±4000) i ponownie stabilizowany; (2) **walidacja
  zawinięć czasem** — skok wcześniej niż ~połowa oczekiwanego czasu przejścia od
  poprzedniego zawinięcia to glitch i jest ignorowany; (3) **cross-check zakresu
  wolt/czas** — czas między dwoma zawinięciami × tempo fazy daje niezależny
  pomiar zakresu; jeśli różni się >2× od pomiaru napięciowego, nachylenie jest
  podejrzane i wygrywa zakres CZASOWY (ns/V przeskalowane do zgodności).

---

## [v0.67-rtos]

### Dodane
- **`LC` sam się przygotowuje przed rampą (wygoda operatora).** Uruchomienie `LC`
  wymagało wcześniej ręcznej sekwencji `LA 7` / `AP` / „poczekaj aż faza dojdzie
  do środka"; start z fazą przy railu był główną przyczyną słabych kalibracji.
  `LC` teraz samodzielnie: (1) uzbraja picDIV do synchronizacji z 1PPS, jeśli
  jest fix GPS, potem (2) czeka do ~60 s, aż napięcie fazy ustali się w
  centralnym pasmie detektora (środek ± ¼ zakresu, utrzymane kilka sekund) przed
  rozpoczęciem rampy. Wypisuje każdy krok i kontynuuje z jasną adnotacją, jeśli
  fazy nie da się wycentrować w czasie. Wystarczy uruchomić `LC` — bez ręcznego
  przygotowania.

---

## [v0.66-rtos]

### Naprawione
- **`LC` mierzy teraz PEŁNY zakres detektora (był ułamek, np. <75 ns).** Dwa
  błędy zaniżały `range_ns` na wąskim detektorze: (1) próg zawinięcia był
  sztywne 0,5 V — większy niż cały ~0,33 V zakres detektora — więc zawinięcia
  nigdy nie były wykrywane; (2) `range_ns` brano z małego wycinka, który faza
  akurat przemiotła podczas rampy, nie z pełnego zakresu jednoznaczności
  detektora. `LC` przemiata teraz aż zobaczy **dwa zawinięcia** (jeden pełny
  cykl), śledzi prawdziwe min/max przez zawinięcia dla zakresu, i wciąż dopasowuje
  nachylenie (ns/V) na czystym segmencie przed zawinięciem. Próg zawinięcia jest
  teraz względny do zakresu detektora. Rampa/okno przestrojone (offset 70 LSB,
  180 s), żeby zmieścił się i długi czysty segment nachylenia, i dwa zawinięcia.
  `LC` raportuje, czy zobaczył 0/1/2 zawinięcia, żebyś wiedział, czy zakres jest
  dokładny, przybliżony, czy dolnym oszacowaniem.

---

## [v0.65-rtos]

### Naprawione
- **DPLL korygował za rzadko dla wąskiego detektora (wyglądał na „zamrożony").**
  DPLL zmieniał PWM tylko co 10 s, a LOCK co `lock_interval_s`; na wąskim
  detektorze faza przemiata cały zakres w ~10-15 s resztkowego dryfu, więc
  między korekcjami faza błądziła i zawijała się, a PWM stał (widoczne jako PWM
  przyklejony do jednej wartości przez 114 próbek). DPLL koryguje teraz co 2 s.
  To *nie* jest błąd schematu: w każdym stanie to PWM (przez filtr RC → EFC)
  steruje OCXO — Vphase jest tylko pomiarem sprzężenia zwrotnego do ADC, więc
  słusznie nie ma analogowego toru Vphase→EFC.
- **Interwał LOCK ograniczony do sensownego zakresu (1..30 s).** Uszkodzony
  `lock_interval_s` (np. 50373 widziane w logu) sprawiłby, że LOCK korygowałby
  mniej więcej raz na 14 godzin; jest teraz ograniczony w czasie działania i w
  komendzie `LIV`, żeby LOCK dalej śledził.

---

## [v0.64-rtos]

### Zmienione
- **Usunięto zawodny auto-probe polaryzacji; polaryzację ustawia się ręcznie.**
  Jednocyklowy probe nie potrafił oddzielić efektu PWM od własnego dryfu fazy na
  wąskim, dryfującym detektorze, więc wielokrotnie wykrywał zły znak (+1 tam,
  gdzie płytka jest −1). ACQ wstrzymuje się teraz i wypisuje przypomnienie o
  uruchomieniu `LPOL -1` (lub `+1`) i `ES`, gdy polaryzacja nieustawiona, a
  DPLL/LOCK i tak się wstrzymują przy nieznanej polaryzacji. Gdy `LPOL` jest
  ustawione i zapisane, wszystkie trzy stany używają go spójnie — to niezawodne,
  czego o probe nie dało się powiedzieć.

---

## [v0.63-rtos]

### Naprawione
- **Wykryta polaryzacja jest teraz współdzielona przez wszystkie trzy stany.**
  Auto-wykryty znak żył w statycznej zmiennej lokalnej wewnątrz ACQ, niewidocznej
  dla DPLL/LOCK, które wpadały w fallback +1 i — na płytce o odwrotnej
  polaryzacji z niezapisanym znakiem — pchały fazę na górny rail, z rosnącym PWM
  i częstotliwością oddalającą się od 10 MHz. ACQ zapisuje teraz wykryty znak do
  `g_ltic.polarity`, więc każdy stan go używa (i wypisuje przypomnienie o `ES`).
- **DPLL/LOCK wstrzymują się zamiast zgadywać, gdy polaryzacja nieznana.** Bez
  ustalonego znaku dają teraz zerową korekcję i pozwalają maszynie wrócić do ACQ
  (który sonduje), zamiast zakładać +1 i uciekać.
- **Strażnik ucieczki.** Jeśli faza jest przyklejona do railu, a PWM zostaje
  wypchnięty o więcej niż ~6000 LSB od punktu startu pętli, pętla zamraża się i
  ostrzega raz („check LPOL / re-centre") zamiast zjeżdżać PWM na skraj i
  rozstrajać OCXO.

### Uwaga
- Zapisz polaryzację: gdy pętla wypisze „detected …polarity -1", uruchom `ES`,
  żeby przetrwała restart (to była główna przyczyna ostatniej ucieczki — znak
  był ustawiony, ale nigdy zapisany, więc wracał do auto/jeden).

---

## [v0.62-rtos]

### Naprawione
- **DPLL i LOCK stosują teraz polaryzację płytki (wcześniej tylko ACQ).** ACQ
  używał wykrytego/wymuszonego znaku `LPOL`, ale DPLL i LOCK nie — więc na
  płytce o odwrotnej polaryzacji sterowały fazą w złą stronę, spychając Vphase
  na dolny rail i wracając od razu do ACQ (faza centrowała się w ACQ,
  przekazywała do DPLL, po czym była pchana do ~0 V i wracała). Wszystkie trzy
  stany dzielą teraz tę samą polaryzację, więc DPLL/LOCK ciągną fazę ku środkowi
  zamiast w rail. Przy działającym już przekazaniu ACQ (v0.61) to właśnie
  pozwala DPLL utrzymać się i przejść do LOCK.

---

## [v0.61-rtos]

### Naprawione
- **ACQ zeruje teraz dryf fazy zamiast gonić jej pozycję.** Przy poprawnej
  polaryzacji (`LPOL -1`) PWM przestał uciekać, ale faza wciąż przemiatała cały
  detektor i zawijała się, więc ACQ nigdy nie spełniał warunku wyjścia „w oknie
  + małe nachylenie". Resztkowy offset częstotliwości (~-0,26 Hz) napędzał fazę
  ~26 ns/s przez detektor 318 ns — dużo za szybko. Dominujący człon ACQ działa
  teraz na DRYFIE fazy (dFaza/dt), sprowadzając offset częstotliwości do zera,
  żeby faza przestała się ruszać; słaby człon centrujący parkuje ją w środku
  dopiero gdy dryf jest już mały. Skoki dryfu od zawinięć (faza skacze >½
  zakresu w kroku) są odrzucane, żeby nie psuły estymaty dryfu ani przejść
  bramkowanych nachyleniem.

---

## [v0.60-rtos]

### Naprawione
- **ACQ uciekał z PWM przy odwrotnej polaryzacji płytki.** ACQ przesuwał PWM w
  stałym kierunku ku `zero_offset`; na sprzęcie, gdzie zwiększanie PWM obniża
  napięcie fazy (odwrotny znak), pchało to PWM coraz niżej, a faza zawijała się
  chaotycznie, więc ACQ nigdy się nie ustabilizował (obserwowane jako długie
  utknięcie w ACQ z PWM zjeżdżającym z ~41000 do ~17000). ACQ **wykrywa teraz
  automatycznie polaryzację PWM→faza** małym krokiem próbnym, potem steruje ku
  celowi z właściwym znakiem. Nowa komenda `LPOL -1/0/1` wymusza znak (0 = auto).
- **ACQ centruje teraz na środku zakresu detektora, nie na `zero_offset`.** Na
  wąskim, niskim detektorze `zero_offset` może siedzieć blisko dna (np.
  0,097 V), więc celowanie w niego trzymało fazę przy railu (ryzyko latch-up /
  zawinięcia, zgodnie z uwagą Dana o wyborze środka skali). ACQ celuje teraz w
  środek zakresu, z możliwością nadpisania przez `LCV <wolty>`.

### Dodane
- Komendy CLI `LPOL` (polaryzacja PWM→faza) i `LCV` (cel centrowania ACQ), obie
  zapisywane w EEPROM i pokazywane przez `LL`.

---

## [v0.59-rtos]

### Zmienione
- **Bramkowanie nachyleniem fazy przy przejściach stanów (algorytm 10).** Za
  radą Dana (time-nuts) oba przejścia stanów LTIC sprawdzają teraz NACHYLENIE
  fazy (dFaza/dt), nie tylko wielkość fazy. Ponieważ częstotliwość to pierwsza
  pochodna fazy, małe nachylenie oznacza, że częstotliwość jest już blisko
  10 MHz — więc ACQ→DPLL wymaga teraz szerokiego okna nachylenia, a DPLL→LOCK
  ~5× węższego, co zapobiega przekazaniu, gdy faza jedynie przelatuje przez
  środek z dużą prędkością (co zalokowałoby złą częstotliwość). LOCK wraca też
  do DPLL, jeśli nachylenie rośnie. To sprawia, że częstotliwość trafia bardzo
  blisko nominału przy każdym przekazaniu.

---

## [v0.58-rtos]

### Naprawione
- **Rampa `LC` zdecydowanie za szybka dla wąskiego detektora.** Na sprzęcie,
  którego detektor obejmuje tylko ułamek zakresu ADC (np. ~0,33 V na okres
  jednoznaczności), stara rampa +2000 LSB przemiatała fazę przez cały detektor
  co ~1-2 s, więc każda próbka trafiała w rail albo zawinięcie i `LC` przerywał
  z "mostly railed". Domyślny offset rampy to teraz łagodne 60 LSB (≈4-5 ns/s
  na typowym OCXO), a `LC` adaptacyjnie zmniejsza offset dalej, jeśli zmierzony
  dryf przekroczyłby detektor w mniej niż ~15 s. Poprawka pomiaru
  częstotliwości z v0.56 potwierdzona (realne df raportowane, np. 1,4-2,0 Hz,
  nie stare zaszyte 0,6).

---

## [v0.57-rtos]

### Naprawione
- **ACQ aktywnie centruje teraz fazę (było tylko sterowanie częstotliwością).**
  Stan ACQ wcześniej korygował tylko błąd częstotliwości TIM2; gdy OCXO był już
  blisko 10 MHz, nic nie napędzało fazy, więc mogła utknąć przy krawędzi
  detektora na zawsze i nigdy nie spełnić warunku wyjścia ACQ→DPLL
  (obserwowane jako całonocne utknięcie z Vphase nisko). ACQ przesuwa teraz PWM
  w stronę środka detektora, gdy odczyt jest na krawędzi, i steruje
  proporcjonalnie do błędu fazy, gdy jest w oknie.
- **Środek fazy brany z kalibracji, nie z zaszytego 1,65 V.** Realny sprzęt
  może mieć wąskie pasmo detektora daleko od środka ADC (np. 0..0,45 V), więc
  pętla centruje teraz na skalibrowanym `zero_offset` (z zgrubnym fallbackiem
  0,22 V) zamiast zakładać 1,65 V. Uruchom `LC`, żeby `zero_offset`/`ns_per_volt`
  odzwierciedlały realne pasmo.

---

## [v0.56-rtos]

### Naprawione
- **Pomiar częstotliwości w `LC`.** Kalibracja czytała średnią częstotliwości
  z okna 10 s raz, zaraz po 10 s settlingu — na realnym sprzęcie to okno nie
  zdążyło jeszcze nadążyć za wymuszoną rampą, więc tempo rampy (a przez to
  `ns_per_volt`) wychodziło błędne. `LC` czeka teraz 30 s, potem próbkuje
  średnią ze 100 s (stabilniejsza, z fallbackiem na 10 s) dwukrotnie w odstępie
  ~5 s i uśrednia.
- **Obsługa sufitu w `LC`.** Próbki, w których napięcie TIC siedzi na suficie
  lub podłodze ADC (faza poza oknem detektora), są teraz pomijane zamiast
  spłaszczać dopasowanie najmniejszych kwadratów, a `LC` przerywa z jasnym
  komunikatem, jeśli rampa jest w większości na sufitach (każąc najpierw
  wycentrować Vphase blisko środka).
- **Poprawka kompilacji:** usunięto zduplikowany extern `g_ltic_voltage` w
  GPSDO_algorithms.cpp, który kolidował z deklaracją w `gpsdo_state.h`.

---

## [v0.55-rtos]

### Dodane
- **Algorytm 10 (trójstanowy PLL LTIC) — pętla jest już zaimplementowana.**
  `LA 10` dyscyplinuje OCXO z fazy sprzętowego TIC (PA1) przez hybrydową
  maszynę stanową ACQ → DPLL → LOCK. ACQ sterowany częstotliwością (TIM2),
  żeby ściągnąć OCXO blisko 10 MHz i faza dryfowała wolno; DPLL dodaje człon
  fazy LTIC do szybkiego centrowania; LOCK sterowany fazą, z wolną
  aktualizacją co `lock_interval_s` i pasmem histerezy do powrotu na DPLL.
  picDIV uzbraja się automatycznie przy wejściu w ACQ. Pętla pracuje w
  nanosekundach, gdy TIC jest skalibrowany (`LC`), a bez kalibracji przechodzi
  na fazę w woltach (nominalnie) z jednorazowym ostrzeżeniem. Stan jest
  zachowywany w `g_ltic.state`, więc ciepły restart (`RB`) wznawia w środku
  sekwencji zamiast zaczynać od ACQ. Pole trendu pokazuje `ACQ` / `DPLL` /
  `LOCK`.
- **Trzeci zestaw PID (ACQ).** `LticParams_t` zyskał PID `acq` obok `dpll` i
  `lock`, z własnymi komendami CLI `AQP` / `AQI` / `AQD` / `AQL` i zapisem w
  EEPROM. `LL` pokazuje teraz wszystkie trzy zestawy.

### Zmienione
- **Układ EEPROM rozszerzony do 216 bajtów (rezerwa do 224).** Blok PID ACQ
  [200..215] dodany pod tym samym podpisem `GPSD2` z guardami NaN/`0xFF`, więc
  starsze zapisy nadal wczytują się z domyślnymi nastawami ACQ.

---

## [v0.54-rtos]

### Dodane
- **`LC` — automatyczna kalibracja LTIC.** Samodzielnie mierzy nachylenie
  napięcie→czas detektora TIC, bez żadnego zewnętrznego wzorca. `LC` wymusza
  mały offset PWM, żeby faza narastała liniowo, wyznacza tempo rampy z błędu
  częstotliwości TIM2 (`phase_rate = df / BASE_FREQ × 1e9` ns/s), dopasowuje
  metodą najmniejszych kwadratów napięcie TIC do czasu (`dV/dt`) i liczy
  `ns_per_volt = phase_rate / (dV/dt)`. Zapisuje też przemiecioną rozpiętość
  napięcia jako `range_ns` oraz `zero_offset` w połowie skali, wykrywając jedno
  zawinięcie, by mieć czysty segment rampy. Działa w zadaniu sterującym jak
  `CT`, z tym samym wzorcem bezpieczeństwa (PWM zapamiętany i przywrócony,
  wyniki z guardami, przerwanie przy braku GPS / za mało punktów / osobliwym
  lub płaskim dopasowaniu — parametry niezmienione przy każdym błędzie). Wyniki
  trafiają do bieżących parametrów LTIC; przejrzyj `LL`, potem `ES` by zapisać.
  Nowe stałe configu `LTIC_CAL_PWM_OFFSET`, `LTIC_CAL_SECS`,
  `LTIC_CAL_MIN_POINTS`. To wypełnia pola kalibracyjne, których będzie
  potrzebować pętla fazy A; sama pętla nadal nie jest zaimplementowana.

---

## [v0.53-rtos]

### Dodane
- **Komendy restartu warm/cold `RB` i `CR`.** `RB` robi ciepły restart
  (`NVIC_SystemReset()`) z zachowaniem EEPROM, więc wciąż ciepły OCXO odtwarza
  swój zdyscyplinowany stan. `CR YES` robi zimny restart: kasuje EEPROM (powrót
  do fabrycznych domyślnych — PWM, model, kalibracja, parametry LTIC) i
  restartuje; potwierdzenie `YES` jest wymagane, bo kasuje wyuczony model OCXO.
- **Infrastruktura algorytmu 10 (LTIC) — parametry, CLI i EEPROM.** Pełny
  zestaw parametrów, edycja CLI i zapis w EEPROM dla planowanego 3-stanowego
  PLL na LTIC (ACQ→DPLL→LOCK), żeby konfiguracja była gotowa zanim powstanie
  sama pętla („faza A"). Nowa struktura `LticParams_t` zawiera kalibrację TIC
  (ns/V, offset zera, zakres), dwa zestawy PID (szerokopasmowy DPLL +
  wąskopasmowy LOCK), progi przejść stanów, interwał LOCK i wznawialny stan.
  Piętnaście komend CLI ustawia/pokazuje te pola (`LL`, `LNV/LZO/LRN`,
  `DPP/DPI/DPD/DPL`, `LKP/LKI/LKD/LKL`, `LAT/LDT/LIV`). `LA 10` jest
  rozpoznawane przez parser, ale zgłasza „not implemented yet" i odmawia
  wyboru, więc OCXO nigdy nie zostaje bez dyscyplinowania. Sama pętla nie jest
  zaimplementowana — to faza A, czeka na sprzęt LTIC.

### Zmienione
- **Układ EEPROM rozszerzony do 200 bajtów (rezerwa do 208).** Blok LTIC
  [144..207] dodany pod **tym samym podpisem `GPSD2`**; każde nowe pole jest
  zabezpieczone guardem NaN/`0xFF`, więc obrazy EEPROM zapisane starszym
  firmware wczytują się czysto, a parametry LTIC przyjmują wartości domyślne do
  czasu ustawienia. Bez migracji ani re-init.

---

## [v0.52-rtos]

### Dodane
- **Podgląd napięcia fazy LTIC (Lars' TIC).** Napięcie TIC na PA1 było już
  próbkowane i wysyłane w telemetrii serial, ale nie miało reprezentacji na
  ekranie. Dodano (wszystko pod `GPSDO_LTIC`, więc zero wpływu na buildy bez
  TIC):
  - **wiersz na TFT** pokazujący `Vph:x.xxxV` (oraz `… NNNns` po kalibracji);
  - **pozycję LTIC w checkliście sprzętu na splashu** (`[x] LTIC phase (PA1)`
    — pokazywana gdy wkompilowana, jak TM1637/TFT, bo TIC jest read-only i nie
    da się go wykryć);
  - **stałą kalibracyjną `LTIC_NS_PER_VOLT`** w configu (0 = nieskalibrowane →
    tylko wolty). Po ustawieniu zmierzonego nachylenia rampy, wyświetlacz i
    planowany algorytm dyscyplinowania fazą przeliczają wolty na ns.
  To **warstwa wyłącznie podglądowa/telemetryczna** — pętla sterowania jeszcze
  nie dyscyplinuje OCXO z TIC (planowane jako osobna faza, nowy algorytm
  oparty na LTIC). OLED/LCD świadomie pozostawiono bez zmian (ich układy są
  pełne); Vphase jest tam dostępne przez logowanie serial, co na tym etapie
  wystarcza do charakteryzacji TIC.

---

## [v0.52-rtos]

### Dodane
- **Warstwa podglądu napięcia fazy LTIC (Lars' TIC).** Gdy `GPSDO_LTIC` jest
  wkompilowane, zatrzaśnięte napięcie TIC (`g_ltic_voltage`, już próbkowane na
  PA1 i rozładowywane co PPS) jest teraz pokazywane jako podgląd: dedykowany
  wiersz `Vph:` na TFT (pod rzędem czujników, widoczny tylko z wkompilowanym
  LTIC) oraz pozycja `LTIC phase (PA1)` w checkliście startowej. Telemetria
  serial już wcześniej zawierała Vphase. Nowa stała kalibracyjna
  `LTIC_NS_PER_VOLT` pozwoli w przyszłym buildzie przeliczyć napięcie na fazę w
  nanosekundach: dopóki wynosi 0 (domyślnie, nieskalibrowane) wyświetlacze
  pokazują tylko wolty; po ustawieniu wiersz TFT pokazuje też `<n>ns`. To tylko
  podgląd/telemetria — pętla sterowania jeszcze nie dyscyplinuje na LTIC; to
  planowany osobny algorytm. Układy OLED/LCD bez zmian (oba pełne); Vphase
  zostanie tam dodane, gdy LTIC stanie się operacyjnym wejściem pętli.

---

## [v0.51-rtos]

### Dodane
- **Komendy CLI są teraz niewrażliwe na wielkość liter.** Dyspozytor komend
  porównywał je przez `strcmp()`, więc `LA` działało, ale `la` już nie.
  Dopasowanie komend używa teraz małej funkcji pomocniczej niewrażliwej na
  wielkość liter (`cli_ieq`), więc akceptowana jest dowolna wielkość liter
  (`LA` / `la` / `La` są równoważne), włącznie z komendami pisanymi małymi
  literami (`up1`, `dp10`, …) oraz rodziną `KP`/`KI`/`KD`/`IL` (gdzie litera
  parametru również jest dopasowywana niewrażliwie). Argumenty komend bez
  zmian; `TO A` już wcześniej akceptowało obie wielkości.

### Zmienione
- **Obsługa ZED-F9T (Gen9) nie jest już eksperymentalna.** Ścieżka survey-in
  CFG-VALSET oraz fallback monitora NAV-SVIN zostały przetestowane na realnym
  sprzęcie przez użytkownika EEVblog danieljw, więc oznaczenia
  „eksperymentalny / nietestowany" zostały usunięte z kodu, configu i plików
  README. Bez zmian w samej ścieżce F9T — tylko jej status.

---

## [v0.50-rtos]

### Dodane
- **Obsługa odbiornika czasowego ZED-F9T (Gen9) — eksperymentalna,
  nietestowana.** Dodano trzecią ścieżkę survey-in obok sprawdzonych LEA-6T /
  LEA-M8T. `ubx_start_survey_in()` wysyła teraz także ramkę `CFG-VALSET`
  (0x06 0x8A) ustawiającą klucze konfiguracyjne Gen9: `CFG-TMODE-MODE`
  (survey-in), `CFG-TMODE-SVIN_MIN_DUR` oraz `CFG-TMODE-SVIN_ACC_LIMIT` (ten
  ostatni przeliczany z mm na jednostkę 0.1 mm odbiornika F9T). Monitor
  survey-in zyskał równoległy parser `NAV-SVIN` (0x01 0x3B) i przechodzi na
  niego, gdy `TIM-SVIN` nie odpowiada, bo generacja F9 raportuje survey-in
  przez NAV-SVIN. ⚠️ Napisane na podstawie dokumentacji u-blox/ubxtool bez
  modułu F9T pod ręką — identyfikatory kluczy, jednostka 0.1 mm i offsety
  payloadu NAV-SVIN NIE są zweryfikowane na sprzęcie. Ramka legacy `CFG-NAV5`
  (tryb stacjonarny) może zostać odrzucona (NAK) przez F9T (nieszkodliwe;
  ścieżka survey-in jest niezależna). Dwa przetestowane odbiorniki bez zmian:
  TIM-SVIN jest nadal próbowany jako pierwszy, więc zachowanie LEA-6T /
  LEA-M8T / NEO-M8T pozostaje niezmienione. Udokumentowane jako eksperymentalne
  w README i config.

### Zmienione
- **Podtytuł splashu LCD 20×4** zmieniony z `GPS-Disciplined Osc.` na
  `GPS-Disciplined OCXO`, zgodnie ze splashem TFT (oba 20 znaków, pełna szerokość).

### Uwagi
- **NEO-M8T** potwierdzony (analizą datasheetu) jako w pełni zgodny z istniejącą
  ścieżką LEA-M8T — ten sam układ M8 + FW3, te same CFG-TMODE2 / TIM-SVIN — bez
  zmian w kodzie. Udokumentowane w sekcji odbiorników czasowych.

---

## [v0.49-rtos]

### Naprawione
- **Kolejność makr w config: `OUT_SERIAL` respektuje teraz `GPSDO_BLUETOOTH`.**
  Makro routingu `OUT_SERIAL` było ewaluowane blisko początku
  `gpsdo_config.h`, *przed* zdefiniowaniem `GPSDO_BLUETOOTH` (i kilku innych
  przełączników funkcji) niżej w pliku. W efekcie `OUT_SERIAL` zawsze
  rozwijało się do USB `Serial`, nawet gdy Bluetooth był włączony, a build z
  zakomentowanym `GPSDO_BLUETOOTH` mógł nie kompilować się zależnie od tego,
  co go używało. Wszystkie przełączniki funkcji są teraz zgrupowane razem
  blisko początku pliku, a makra od nich pochodne (`OUT_SERIAL`) ewaluowane
  później, w dedykowanej sekcji „Derived macros". Brak zmian funkcjonalnych
  poza tym, że wyjście Bluetooth faktycznie trafia teraz na Serial2. Skan
  pozostałych plików źródłowych nie wykrył innych problemów z kolejnością
  definicja-po-użyciu.

### Zmienione
- **Ujednolicono wzorzec startowy HT16K33 z TM1637.** Po starcie HT16K33
  pokazuje teraz `----` (kreski na segmencie G) zamiast `oooo`, zgodnie ze
  wzorcem startowym TM1637 — oba zegary LED sygnalizują „żywy, oczekiwanie na
  GPS" tak samo. Wskaźnik `oooo` pozostaje dla przypadku braku fixa podczas
  pracy (gdzie TM1637 też pokazuje `oooo`), więc oba wyświetlacze zachowują
  się teraz identycznie w każdym stanie.
- **Linia kredytów na splashu TFT** zmieniona z `jmnlabs + with Claude
  (Anthropic)` na `jmnlabs with Claude (Anthropic)` (usunięto `+`).

---

## [v0.48-rtos]

### Dodane
- **Obsługa TFT ILI9488 480×320 SPI (`GPSDO_TFT_ILI9488`).** ⚠️ Nietestowany —
  brak panelu do testów. Istniejący ekran roboczy 320×240 ILI9341/ST7789 oraz
  animowany splash są współdzielone i automatycznie skalowane do 480×320
  podczas kompilacji: szerokość ×1.5 i wysokość ×1.33 przez niezależne makra
  `TFT_SX`/`TFT_SY` (proporcje panelu różnią się od czystego 1.5×), a fonty
  TFT_eSPI mapowane o rozmiar w górę przez `TFT_F`. Geometria zweryfikowana,
  że mieści się w panelu; jeszcze nieuruchomiony na realnym sprzęcie. Ustaw
  `ILI9488_DRIVER` + `TFT_WIDTH 320`/`TFT_HEIGHT 480` (+ `LOAD_FONT6`) w
  `User_Setup.h` biblioteki TFT_eSPI.
- **Mostek SPI→T6963C jako nowy backend wyświetlacza (`GPSDO_T6963C`).**
  ⚠️ Eksperymentalny / niesprawdzony — backend jest kompletny i kompiluje
  się, ale połączenie nie zostało jeszcze zweryfikowane na czystym sprzęcie
  (uruchamianie na długich przewodach pokazało dzwonienie i fałszywe zbocza
  CS; to samo na masterze referencyjnym → problem integralności sygnału, nie
  firmware). Domyślnie wyłączony; zostaw wyłączony do testu na krótkim
  okablowaniu point-to-point.
  Obsługuje panel PowerTip PG240128 (240×128 mono) przez zewnętrzny
  `T6963C_SPI_bridge` po SPI1, używając wysokopoziomowych komend rysowania
  (`T6963C_Bridge.h`). Wybierany w konfiguracji jak pozostałe wyświetlacze;
  wzajemnie wykluczający się z TFT (wspólne piny SPI1 / slot wyświetlacza).
  - Reużywa pinów SPI1 TFT: `SCK PA5`, `MOSI PA7`, `CS PB13`, `READY PB12`;
    zwalnia `PB15` (był TFT_RST).
  - Skondensowany układ 240×128 odzwierciedlający ekran TFT: nagłówek
    (tytuł + czas LMT), duża częstotliwość (fonty LOGISOSO), wiersz statusu,
    wiersze wartości (PWM/Vctl, INA219, czujniki) i pasek postępu survey-in.
  - Panel monochromatyczny → wskazanie koloru lock/holdover staje się
    odwróconym (wypełnionym) prostokątem wokół słowa statusu (`LOCK` /
    `HOLD` / `H-LOST` / `NOFIX`).
  - Jedna wsadowa transakcja SPI na odświeżenie (jedno oczekiwanie na READY),
    z auto-podziałem biblioteki mostka jako zabezpieczeniem; cache zmian
    per-pole pomija zbędne przerysowania.
  - Statyczny splash startowy (logo + podtytuł + checklista sprzętu); bez
    animacji fali, bo renderowanie wsadowe przez SPI byłoby kosztowne na
    małym panelu mono.

---

## [v0.47-rtos]

### Dodane
- **Komenda CLI `SV`** — włącza/wyłącza survey-in (Time Mode) na module
  czasowym w czasie pracy, zapisywana w EEPROM (bajt 143). `SV` pokazuje
  stan, `SV 0` wyłącza (pozostań w trybie nawigacji — przydatne do testów na
  biurku), `SV 1` włącza; `ES` zapisuje, stosowane przy następnym starcie.
  Domyślnie włączone na świeżym EEPROM.

### Naprawione
- **Polling survey-in nie blokuje już wyświetlaczy.** `ubx_poll_svin()`
  czekał do 1000 ms aktywnym `delay()`, głodząc rodzeństwo wysokopriorytetowego
  zadania GPS — wyświetlacze wyraźnie się opóźniały (najgorzej przy wolniej
  odpowiadającym LEA-6T). Poll używa teraz ~500 ms okna, które ustępuje przez
  `vTaskDelay()` między odczytami, więc zadanie wyświetlacza działa normalnie,
  a okno wciąż niezawodnie łapie odpowiedź TIM-SVIN modułu (latencja
  100-200 ms). Bajty NMEA widziane podczas skanowania są przekazywane do
  TinyGPS++, więc fix nie jest zakłócany. Gdy survey już odpowiedział,
  sporadyczne nieudane odczyty nie przerywają monitora; dziury w sekwencji
  `svin dur=` zniknęły.
- **Survey-in wychodzi teraz niezawodnie po spełnieniu warunków.**
  Zakończenie jest ogłaszane, gdy ALBO odbiornik oznaczy średnią pozycję
  jako ważną, ALBO spełnione są kryteria użytkownika (dokładność ≤ limit
  ORAZ czas ≥ minimum) — niektóre odbiorniki (zwłaszcza LEA-6T) osiągały
  ~0,45 m długo po minimum, ale pozostawiały survey „aktywny", więc stary
  test `valid && !active` nigdy się nie wyzwalał. Bezpiecznik wynosi teraz
  `3 × SVIN_MIN` (min. 600 s), więc wolno zbiegający się survey na słabej
  antenie ma uczciwą szansę.
- Dokładność TIM-SVIN we wczesnej fazie survey (`0xFFFFFFFF` = „brak
  oszacowania") jest ograniczana do 65535 mm zamiast się przepełniać.

### Zmienione
- **Dokładność TFT**: INA219 pokazuje teraz napięcie szyny z 3 miejscami i
  prąd z 2 miejscami; napięcie sterujące PWM (Vctl) z 3 miejscami.

### Dokumentacja
- README (EN/PL) zaznacza, że survey-in wymaga dobrej anteny zewnętrznej z
  pełnym widokiem nieba, i odnotowuje obserwację z testów, że LEA-6T jest
  czulszy niż LEA-M8T w trudnych warunkach. Oba moduły zweryfikowano —
  kończą survey-in i przechodzą w Time Mode na profesjonalnej antenie
  zewnętrznej (geodezyjnej). Poprawiono kilka nieaktualnych komentarzy w
  kodzie (rozmiar EEPROM 144 B, TIM-SVIN zamiast NAV-SVIN).

---

## [v0.46-rtos]

### Usunięte
- **Całkowicie usunięto wybór OCXO w czasie kompilacji (CTI / Vectron).**
  Komenda `CT` mierzy wzmocnienie obiektu i wylicza wszystkie współczynniki
  dla dowolnego zamontowanego oscylatora, więc definicje per-OCXO, tabele
  PID i przełącznik `DEFAULT_PWM` nie są już potrzebne. Pętla startuje od
  uniwersalnej wartości środkowej PWM (32767 ≈ 1,65 V) przed pierwszym `CT`.

### Dodane
- **Wielowariantowy start survey-in.** LEA-6T i LEA-M8T akceptują różne
  komendy Time Mode (obie zweryfikowane w u-center), więc firmware próbuje
  każdej po kolei i zatrzymuje się na pierwszym ACK: `CFG-TMODE2` 0x06 0x3D
  (LEA-M8T), a następnie klasyczny `CFG-TMODE` 0x06 0x1D (LEA-6T, u-blox 6).
  Samo dostosowuje się do obu modułów. Jeśli żaden nie zostanie
  zaakceptowany, moduł jest uznawany za już timujący i mimo to monitorowany.

### Naprawione
- **Dokładność TIM-SVIN była bezsensowna (pokazywała ~467 km).** Pole
  `meanV` to *wariancja* pozycji w mm², nie odległość — firmware bierze
  teraz jej pierwiastek, by raportować dokładność 1-sigma w mm (zweryfikowane
  względem u-center: 18113534 mm² → ~4,3 m). Czas/dokładność survey-in mają
  teraz sensowne wartości.
- **Zawieszenie startu, gdy survey-in faktycznie ruszył (LEA-M8T).** Pętla
  postępu survey-in działała wewnątrz `gpsdo_gps_init()` — przed startem
  schedulera — i używała `vTaskDelay()`, co zawiesza system, gdy wywołane
  przed `vTaskStartScheduler()`. Nie ujawniało się na LEA-6T, bo ten NAK-uje
  CFG-TMODE2 i pomijał pętlę; M8T ACK-uje, wchodził w pętlę i zamarzał
  (niebieski LED zatrzymany). Survey-in teraz tylko *startuje* w init;
  postęp jest pollowany nieblokująco z `vGpsTask` po starcie schedulera.
- **Sporadyczne zawieszenie startu / czarne wyświetlacze** — `STACK_DISPLAY`
  zwiększony z 768 do 1024 słów. Skalowanie fontów i pętla czyszczenia OLED
  sprawiły, że 768 było na granicy; bez haka wykrywającego przepełnienie
  stosu objawiało się to cichym, niedeterministycznym zawisem.
- **Moduł czasowy LEA-M8T teraz działa.** Tkwił w 3D fix nawigacyjnym
  (HDOP ≈ 1), bo firmware wysyłał mu `CFG-TMODE3`, którego jego firmware
  (TIM 1.10, PROTVER 22) nie obsługuje. u-center potwierdził, że LEA-M8T
  używa **tych samych** komunikatów `CFG-TMODE2` / `TIM-SVIN` co LEA-6T.
  Ścieżka czasowa została ujednolicona do jednej implementacji TMODE2;
  osobne opcje `GPSDO_GPS_LEA6T` / `GPSDO_GPS_LEA8T` zastąpiono jedną
  `GPSDO_GPS_TIMING`, a gałąź TMODE3 / NAV-SVIN usunięto.
- **OLED**: dolna połowa dużego napisu `GPSDO` ze splasha (rysowanego
  czcionką dwurzędową) pozostawała za zegarem LMT — przy końcu splasha ekran
  jest czyszczony, każdy rząd wymazany, czcionka 2x2 zresetowana, a cache
  rzędów unieważniony. Napisy `GPSDO` i wersja są wycentrowane; stopka
  używa `jmnlabs+Claude`.
- **LCD 20x4**: linia tytułu/wersji przesunięta w prawo (dwie spacje), by
  sufiks `-rtos` nie był ucinany.
- Poprawiono komentarz nagłówka układu EEPROM (143 bajty, było błędnie 134).

### Zmienione
- **TFT**: biała wartość częstotliwości używa czcionki o stałej szerokości
  (font 1, rozmiar 3), więc jej cyfry zachowują stałą pozycję kolumn;
  podtytuł powiększony i zmieniony na `GPS-Disciplined OCXO`; logo, podtytuł
  i animacja zbiegających się fal podniesione; checklista sprzętu pojawia
  się wolniej z pauzą wstępną, aby nie umknęły pierwsze pozycje; stopka z
  `+`. Wartości czujników (temperatura BMP/AHT, ciśnienie, wilgotność)
  pokazują teraz dwa miejsca po przecinku.

---

## [v0.45-rtos]

### Zmienione
- **Ponownie przebudowany splash TFT** jako metafora złapania fazy: napis o
  twórcach rysowany jest najpierw i pozostaje; dwie sinusoidy 2px (niebieska
  u góry, bursztynowa u dołu) startują z widocznym przesunięciem fazy i małym
  odstępem pionowym, po czym powoli się zbiegają aż się pokrywają i łączą w
  jedną zieloną falę 4px, utrzymaną ~1,8 s. Następnie pojawia się checklista
  sprzętu.
- Czytelny raport serial pokazuje teraz `HDOP:TIME` w trybie czasowym (format
  maszynowy z tabulatorami zachowuje wartość liczbową do wykresów).

### Usunięte
- Zbędne definicje `SERIAL_*_BUFFER_SIZE` w `gpsdo_config.h` (i tak nigdy nie
  docierały do rdzenia). Rozmiary buforów są wyłącznie w `build_opt.h`
  (`RX=256, TX=512`).

---

## [v0.44-rtos]

### Dodane
- **`build_opt.h`** powiększający bufory szeregowe RX/TX do 256 bajtów
  (`-DSERIAL_RX_BUFFER_SIZE=256 -DSERIAL_TX_BUFFER_SIZE=256`). STM32duino
  stosuje te flagi do całej kompilacji łącznie z rdzeniem, czego `#define`
  w szkicu nie potrafi osiągnąć. Zapobiega to gubieniu i sklejaniu zdań
  NMEA przy 38400 baud, gdy zadanie GPS zostanie na chwilę wywłaszczone
  (przyczyna zniekształconego NMEA z LEA-6T).

### Zmienione
- **Przebudowany splash powitalny TFT**: dwie sinusoidy w różnych kolorach
  (niebieska z lewej, bursztynowa z prawej) zbiegają się do środka i łączą
  w jedną zieloną falę 10 MHz — metafora synchronizmu — z logo GPSDO i
  checklistą sprzętu poniżej. Wydłużone czasy dla czytelności.

### Uwagi
- Zachowane są tylko zdania GGA + RMC (GLL/GSA/GSV/VTG wyłączone), co wraz
  z większym buforem utrzymuje ruch na magistrali z dużym zapasem.

---

## [v0.43-rtos]

### Dodane
- **Wykrywanie Time Mode / `HDOP:TIME`.** Odbiornik czasowy w trybie
  time-only utrzymuje zamrożoną, ważną pozycję, ale raportuje HDOP ≈ 99,99.
  Zamiast pokazywać tę bezsensowną liczbę, wyświetlacze pokazują teraz
  `HDOP:TIME`, gdy ważna pozycja zbiega się z nieistotnym HDOP (≥ 50,00).
  Nowa flaga `gGps.time_mode`.

### Zmienione
- **NAK survey-in obsługiwany łagodnie.** Niektóre moduły czasowe (np.
  egzemplarze z odzysku z zapisaną konfiguracją Time Mode) NAK-ują
  `CFG-TMODE2/3`. Firmware nie traktuje tego już jako błędu — zapisuje, że
  moduł może już pracować w trybie czasowym, i kontynuuje; wykrywanie Time
  Mode w czasie pracy raportuje rzeczywisty stan.
- Wydłużono czasy splasha powitalnego (TFT ~7 s, OLED/LCD ~4,5 s), aby
  ekran powitalny dało się przeczytać.

### Naprawione
- Stopka splasha OLED nie ucina już ostatniego znaku (`jmnlabs/Claude`,
  usunięto spacje wokół ukośnika, by zmieścić się w 16 kolumnach).

---

## [v0.42-rtos]

### Naprawione
- **Błąd kompilacji w kodzie survey-in** (`get_ubx_ack` wywoływane z
  class/id/timeout zamiast wskaźnika na bufor komunikatu, którego oczekuje).
  Obie gałęzie `ubx_start_survey_in` przekazują teraz bufor ramki, zgodnie
  z sygnaturą funkcji. Buildy z modułami LEA znów się kompilują.

### Uwagi
- Moduł czasowy u-blox M8 (**LEA-M8T**) to ta sama generacja co 8T i używa
  CFG-TMODE3 / NAV-SVIN — włącz dla niego `GPSDO_GPS_LEA8T`.

---

## [v0.41-rtos]

### Dodane
- **Animowany splash powitalny na TFT**: przebiegająca sinusoida 10 MHz,
  logo GPSDO oraz checklista sprzętu odtworzona z rzeczywistych flag
  detekcji (moduły pokazują `[x]` / `[ ]`), z dyskretną stopką
  `jmnlabs · with Claude (Anthropic)`. Odtwarzany raz, potem rysowany jest
  ekran roboczy.
- **Splash powitalny na OLED** (tryb znakowy, U8x8): podwójnej wielkości
  `GPSDO`, wersja, linia akcentu i stopka.
- **Splash powitalny na LCD 20x4**: czterowierszowe powitanie z tytułem,
  podtytułem i stopką.

### Naprawione
- **TFT nie aktualizował PWM / Vctl podczas kalibracji.** Wyświetlacz
  kończył działanie zaraz po narysowaniu odliczania, zamrażając siatkę
  informacji. Teraz przechodzi dalej, więc komórka PWM/Vctl aktualizuje się
  na żywo podczas `C` / `CT` — zgodnie z zachowaniem OLED.

---

## [v0.40-rtos]

### Dodane
- **Obsługa odbiorników czasowych LEA-6T / LEA-8T** (`GPSDO_GPS_LEA6T` /
  `GPSDO_GPS_LEA8T`). Na tych modułach firmware wykonuje survey-in przy
  każdym uruchomieniu (CFG-TMODE2 na 6T, CFG-TMODE3 na 8T), po czym
  odbiornik przechodzi w tryb time-only o stałej pozycji z wyraźnie
  czystszym 1PPS. Survey-in kończy się, gdy osiągnięty zostanie minimalny
  czas (`GPSDO_SVIN_MIN_SECS`, domyślnie 120 s) lub próg dokładności
  (`GPSDO_SVIN_ACC_LIMIT`, domyślnie 2000 mm).
- Postęp survey-in jest pokazywany na każdym wyświetlaczu (`SVIN nnns nnm`
  na OLED/LCD/TFT, kreski na zegarach LED), za pomocą nowego stanu
  `g_svin_*`.
- Pozycja jest nadal nadawana w NMEA przez cały czas trybu Time Mode, więc
  wyświetlanie lokalizacji i automatyczna strefa czasowa (`TO A`) działają
  dalej — w oparciu o uśrednioną, zamrożoną pozycję z survey-in.
- `CHANGELOG.md` (i ta wersja PL) są teraz dołączane do archiwum projektu.

### Uwagi
- Zachowanie modułów NEO-6M / NEO-8M jest niezmienione (gdy żadna opcja LEA
  nie jest zdefiniowana).

---

## [v0.39-rtos]

### Dodane
- Rozgrzewanie OCXO jest teraz pokazywane na każdym wyświetlaczu z odliczaniem
  na żywo (`WARMUP nnn s` na OLED/LCD/TFT, kreski na TM1637/HT16K33), w oparciu
  o nowy stan `g_warmup_active` / `g_warmup_remaining`.

---

## [v0.38-rtos]

### Naprawione
- **Dłubanie PWM w stanie ustalonym na algorytmach fazowych (4, 5, 7, 8).**
  Strefa martwa sprawdza teraz także zakumulowaną fazę, nie tylko błąd
  częstotliwości: gdy `|e| < 1 mHz` i `|faza| < 5 Hz·s` (≈500 ns), pętla
  utrzymuje PWM i pokazuje `hit`, więc zalockowany oscylator przestaje być
  szarpany szumem GPS co okres. Mały szum fazy jest trzymany; prawdziwy dryf
  nadal korygowany.
- Wszystkie algorytmy fazowe faktycznie pokazują trend `hit` po zlockowaniu;
  algorytmy FLL (3, 6) otrzymały odpowiednik trzymania locka tylko na
  częstotliwości.
- Odczyty PWM i Vctl na wyświetlaczach aktualizują się teraz na żywo
  **podczas** kalibracji `C` / `CT` (nowa funkcja `wait_secs_pwm` publikuje
  PWM i sampluje ADC Vctl co sekundę, gdy główna pętla jest zajęta).

---

## [v0.37-rtos]

### Zmienione
- `LP 8` i `LP 9` pokazują teraz, skąd te algorytmy faktycznie czytają swoje
  wzmocnienia: algo 8 (hybryda) używa `g_pid[6]` (gałąź FLL) + `g_pid[7]`
  (gałąź PLL); algo 9 (NN) używa stałych wag sieci, więc liczą się tylko
  `NS` / `IL`. Zapobiega to myleniu pustego `g_pid[8]/[9]` z „nienastrojonym"
  po `CT`.

---

## [v0.36-rtos]

### Dodane
- Postęp kalibracji pokazywany na wszystkich wyświetlaczach: odliczanie
  `CAL nnn s` w polu częstotliwości (OLED/LCD/TFT) i `CAL` na zegarach LED
  (TM1637 / HT16K33), za pomocą `g_calib_active` / `g_calib_remaining`.

---

## [v0.35-rtos]

### Dodane
- **Komenda `CT` (Calibrate & Tune).** Mierzy wzmocnienie obiektu `K` z
  trzypunktowego przemiatania PWM (1,5 / 2,0 / 2,5 V) regresją liniową,
  znajduje PWM dla dokładnie 10 MHz i wylicza współczynniki PID dla
  wszystkich algorytmów z `K` (PLL: `Kp = 0,40/K`; FLL: `Kp = 0,35/K`,
  `Ki = Kp/300`, `Kd = Kp·73`; NN: `max_step = 0,05/K`). Z kontrolą
  poprawności, nieniszcząca; `ES` zapisuje wynik.

---

## [v0.34-rtos]

### Zmienione
- **Dwuczasowe strojenie PLL pod „szybkie złapanie, łagodne pilnowanie
  fazy".** Człon dominujący działa na błąd częstotliwości (`Kp ≈ 0,4/K`) dla
  szybkiego wejścia bez przeregulowania; małe człony fazowe usuwają powolny
  dryf. Wspólny stopień wyjściowy dodaje ograniczenie szybkości narastania
  (≈12 LSB/krok dla PLL, 40 dla hybrydy) i strefę martwą blisko locka, więc
  duży nocny dryf fazy jest rozkładany na kilka okresów zamiast jednego
  wielkiego skoku PWM.

---

## [v0.33-rtos]

### Naprawione
- **Algorytm 9 (NN) uciekał w górę.** Poprzednie „wytrenowane" wagi miały duży
  bias wyjścia (≈ −0,96 przy zerowym błędzie → stałe narastanie PWM). Zastąpione
  analitycznie skonstruowaną, pozbawioną biasu, nieparzyście symetryczną siecią:
  zerowe wejście daje dokładnie zerowe wyjście.
- **Algorytmy 4 / 5 / 7 oraz gałąź PLL algo 8 dryfowały.** Używały kroczącej
  średniej okna jako namiastki fazy, która opóźniała aktualizację 10 s o
  500–1000 s i nakręcała integrator. Zastąpione prawdziwą akumulacją fazy
  (`faza += (avg10 − 10 MHz)·10 s`, dokładna liczba cykli), ze sprzężeniem
  o opóźnieniu 10 s.
- Komunikat `GPS fix acquired` odróżnia teraz pierwszy fix po starcie od
  prawdziwego odzyskania po utracie fixa.

### Dodane
- **Automatyczna strefa czasowa (`TO A`).** Czas lokalny podąża za pozycją GPS:
  kompaktowy zestaw reguł stref cywilnych Europy plus reguła DST UE, albo
  strefa słoneczna `round(lon/15)` poza Europą. `TO <n>` zachowuje tryb ręczny.
  Tryb zapisywany do EEPROM (bajt 142, razem 143 bajty) i przywracany przy
  starcie.

---

## [v0.32-rtos]

### Naprawione
- **Raport detekcji sprzętu.** Dodano odporną sondę I2C z podwójną
  weryfikacją (ACK adresu + odczyt 1 bajtu). OLED i HT16K33 były wcześniej
  raportowane jako `OK` bezwarunkowo / na zawodnym ACK; teraz zgłaszają
  rzeczywistą obecność. TM1637 i TFT oznaczone jako `enabled (write-only —
  not verifiable)`.
- **Kolor częstotliwości TFT.** Zielony kolor „zlockowany" wynika teraz z
  rzeczywistego odchylenia od 10 MHz (≤1 mHz na oknie 10000 s lub ≤10 mHz na
  1000 s), niezależnie od algorytmu — więc zalockowany algo 8 też zmienia kolor
  na zielony, a nie tylko przy rzadko emitowanym trendzie `hit`.

---

## [v0.31-rtos]

### Dodane
- **Obsługa 4-cyfrowego zegara HT16K33** (I2C 0x70): samodzielny sterownik
  (HH:MM z migającym dwukropkiem, `oooo` podczas szukania), współdzielący
  magistralę z LCD — bez dodatkowych pinów. TM1637 zachowany.
- Ujednolicony raport sprzętowy przy starcie: każde opcjonalne urządzenie
  zgłasza `OK` lub `not found` w spójnym formacie `HW:`.
- Nowy diagram architektury sprzętu w obu plikach README (TFT + HT16K33).

---

## [v0.30-rtos]

### Dodane
- **Obsługa TFT 240×320 (ILI9341 / ST7789)** przez TFT_eSPI na sprzętowym SPI1
  (SCK PA5, MOSI PA7, RES PB15, DC PB12, CS PB13). Układ poziomy: pasek
  nagłówka, duża częstotliwość z kodowaniem kolorem, dwukolumnowa siatka
  informacji, wiersz czujników i pasek statusu z kodowaniem kolorem.
  Selektywne przerysowywanie komórek minimalizuje ruch SPI. Stos DisplayTask
  podniesiony do 768 słów gdy TFT włączony. Oba sterowniki przetestowane na
  sprzęcie.

---

## [v0.29-rtos]

### Naprawione
- **Synchronizacja picDIV.** Uzbrojenie jest teraz odraczane do pojawienia się
  fixa GPS (zatrzymany divider bez 1PPS na Sync zawiesiłby się z martwym
  wyjściem); dedykowana flaga zastępuje wartownika opartego na znaczniku millis
  (odporna na przepełnienie); usunięto auto-uzbrojenie po kalibracji (pętla nie
  zbiegła jeszcze). Dodano czytelny feedback na serialu. README dokumentuje
  random-walk fazy FLL vs lock fazy PLL dla długoterminowego wyrównania 1PPS.

---

## [v0.28-rtos]

### Naprawione
- **Zakres PWM przy DAC 3,3 V.** PWM STM32 osiąga tylko 0–3,3 V z zakresu EFC
  0–4 V (82,5%), więc dostępne dostrajanie to −10…+14,75 Hz (CTI) i
  −20…+13 Hz (Vectron). Domyślny PWM skorygowany per-OCXO: 32767 (CTI, środek
  1,65 V) i 39718 (Vectron, nominał 2,0 V).

---

## [v0.27-rtos]

### Naprawione
- **Parametry Vectron C4550A1-0213.** Skorygowane do rzeczywistego punktu
  pracy: zasilanie 5 V, EFC 0–4 V, Kv = 10 Hz/V (0,504 mHz/LSB), współczynnik
  skali 1,333 vs CTI (wzmocnienia × 0,75), wspólny domyślny PWM.

### Zmienione
- `README_EN.md` przemianowany na `README.md` (domyślny dla GitHub);
  `README_PL.md` bez zmian.

---

## [v0.26-rtos]

### Dodane
- **Wybór OCXO** w `gpsdo_config.h` (`GPSDO_OCXO_CTI_OSC5A2B02` /
  `GPSDO_OCXO_VECTRON_C4550`), z domyślnymi parametrami PID i domyślnym PWM
  per-OCXO ustalanymi w czasie kompilacji. Awaryjnie używa wartości CTI, gdy
  żaden nie jest wybrany.
- `SP`, `F`, `C`, `T` udokumentowane w tekście pomocy i plikach README.

---

## [v0.25-rtos]

### Dodane
- `g_pressure_offset` (`PO`) i `g_altitude_offset` (`AO`) są teraz zapisywane
  do i przywracane z EEPROM (bajty 134–141, razem 142 bajty).
- Komenda `V` rozszerzona o pełne informacje o autorach/podziękowaniach i
  linki do GitHub.

---

## [v0.24-rtos]

### Naprawione
- **Wyjście Bluetooth.** Wszystkie komunikaty runtime przechodzą przez makro
  `OUT_SERIAL` (Serial2 gdy zdefiniowane `GPSDO_BLUETOOTH`, inaczej USB Serial).

### Dodane
- Pauza/wznowienie raportów (`RP` / `RR`) do wyciszenia strumienia danych
  podczas konfiguracji.
- Parametry PID algorytmów zapisywane do EEPROM (sygnatura `GPSD2`).
- Profesjonalna dokumentacja nagłówków we wszystkich plikach źródłowych;
  README napisane od zera (opis projektu, zasada działania sprzętu,
  architektura oprogramowania) po polsku i angielsku; URL GitHub dodany do
  każdego pliku i do banera serial.

---

## [v0.23-rtos]

### Dodane
- **Strojenie PID w czasie pracy przez CLI** — `LP`, `KP`, `KI`, `KD`, `IL`
  dla algorytmów 3–7, `BC` / `BS` dla mieszania algo 8, `NS` dla kroku sieci
  NN algo 9. Współczynniki przeniesione do globalnej tablicy `g_pid[10]`.

---

## [v0.22-rtos]

### Dodane
- Maszyna 4-stanowa żółtej LED (off / on / wolny puls = holdover ręczny /
  szybki puls = auto-holdover) oraz automatyczny holdover przy utracie fixa GPS
  ze wskaźnikami `H` / `A` na OLED i LCD.

---

## [v0.21-rtos]

### Dodane
- Zegar w wierszu 0 OLED (czas lokalny + dzień tygodnia) po splashu wersji;
  rotujący widok data/dzień w wierszu 2 LCD. Funkcje pomocnicze dnia tygodnia
  (Zeller) i przesunięcia czasu lokalnego.

---

## [v0.20-rtos]

### Zmienione
- Ujednolicone 4-znakowe ciągi trendu; skorygowane formatowanie częstotliwości
  OLED/LCD; zabezpieczenie kompilacji przed jednoczesnym LCD + TM1637;
  poprawiony URL źródła André Balsy.

---

## [v0.19-rtos]

- Pierwsza śledzona baza portu FreeRTOS: STM32F411CE BlackPill, pomiar
  częstotliwości przez TIM2 ETR + przechwytywanie 1PPS TIM3, uśrednianie w
  buforze pierścieniowym, pętla dyscyplinująca PWM-DAC, parsowanie GPS/NMEA,
  wyświetlacze OLED / LCD / TM1637, opcjonalne czujniki AHT/BMP/INA oraz
  początkowe algorytmy sterowania.
