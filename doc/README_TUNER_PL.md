# GPSDO Tuner

Konsola na PC do strojenia pętli na żywo i obserwowania, co robi: trzy
przewijające się wykresy, po jednej zakładce na grupę parametrów oraz pole
komend ręcznych na wszystko, czego zakładki nie obejmują.

To **narzędzie do strojenia**, nie przyrząd pomiarowy — przed wyciąganiem
wniosków z tego, co pokazuje, przeczytaj *Ograniczenia* poniżej.

---

## Wymagania

**Python** 3.9 lub nowszy oraz cztery pakiety:

```
pip install PySide6 pyqtgraph pyserial tzdata
```

| Pakiet | Do czego |
|--------|----------|
| PySide6 | interfejs użytkownika Qt |
| pyqtgraph | wykresy na żywo |
| pyserial | komunikacja z płytką |
| tzdata | wyłącznie przycisk **Generate tz_table.h** |

`tzdata` jest opcjonalne, jeśli nigdy nie używasz tego przycisku, a na Linuksie
i macOS system dostarcza te same dane stref. Na Windows to jedyne źródło, bo
system nie zawiera bazy IANA. Późniejsza aktualizacja: `pip install -U tzdata`.

Uruchom przez `python gpsdo_tuner.py` albo dwuklikiem w Windows: otwierające się
okno konsoli jest automatycznie minimalizowane na pasek zadań i pozostaje
dostępne, gdyby trzeba było odczytać komunikat błędu.

---

## Zgodność wersji

Tuner ma `TOOL_VERSION` śledzące wydanie firmware, dla którego powstał. Przy
połączeniu odczytuje wersję z płytki i porównuje:

- **zgodność** — pasek stanu pokazuje `connected — firmware vX.YZ`
- **niezgodność** — pasek stanu i monitor surowy mówią o tym wprost

Niezgodność nie jest błędem krytycznym i tuner nadal rozmawia z płytką, ale
należy się spodziewać dziwnie czytanych pól albo odrzucanych komend: starszy
tuner nie zna nowszej telemetrii, a nowszy może wysyłać komendy, których płytka
nigdy nie widziała. Używaj pary, która przyszła razem.

---

## Zakładki

| Zakładka | Przeznaczenie |
|----------|---------------|
| **LTIC (algo 10)** | PID per stan trójstopniowej pętli fazowej oraz kalibracja detektora |
| **LTIC-Lars (algo 11)** | Parametry ciągłej pętli PI (`LG`, `LD`, `LTC`, …) |
| **FA damping** | Okno uśredniania członu tłumiącego, per stan |
| **PID algo 3-9** | Kp / Ki / Kd / I_LIMIT dla algorytmów częstotliwościowych |
| **Calibration** | `LC`, `CT` i stałe detektora |
| **Raw monitor** | Wszystko, co wysyła płytka, bez parsowania |
| **Help** | Pełny wykaz komend firmware |

Każda grupa parametrów jest odczytywana przy połączeniu, więc panele startują
wypełnione, a nie puste.

---

## Wykresy

Trzy okna, odświeżane raz na sekundę. To, co pokazują dwa górne, zależy od tego,
jaki algorytm zgłasza płytka:

| | Algorytmy 10 / 11 (LTIC) | Algorytmy 0-9 |
|---|---|---|
| Górne | Faza `dph` (ns) | Wyuczony dryf (LSB) |
| Środkowe | `Vphase` detektora (V), z liniami pasma | Napięcie sterujące `Vctl` (V) |
| Dolne | Błąd częstotliwości (Hz) | Błąd częstotliwości (Hz) |

Tylko pętle LTIC mają detektor fazy, więc przy każdym innym algorytmie te dwa
okna stałyby puste przez całą sesję. Zamiast tego są przekierowane na inne
wielkości, a tytuły zmieniają się automatycznie — nie ma czego przestawiać.

### Span i Follow

**Span** ustala, ile historii jest widoczne: 1 min, 5 min, 15 min, 1 h albo
*all*. Z wybranym oknem wykres przewija się w lewo ze stałą skalą, zamiast
rozciągać oś na cały bufor.

**Follow live** trzyma okno przypięte do najnowszej próbki. Przeciągnij wykres
lub przewiń kółkiem, a opcja sama się odznaczy, oddając oś myszy — wtedy można
przejrzeć cały bufor. Zaznacz ją ponownie (albo zmień Span), by wrócić na żywo.

**Clear plots** odrzuca wszystkie zbuforowane próbki i restartuje oś czasu od
zera — przydatne po nieudanym starcie i szybsze niż restart narzędzia, przy
którym traci się połączenie. Czyści bufory razem z krzywymi, więc nic nie wróci
później na wykres.

**About** odtwarza animację startową, bez powodu innego niż przyjemność.

---

## Ograniczenia

**Historia sięga 30 godzin.** Tuner trzyma 108 000 próbek przy telemetrii 1 Hz.
To pokrywa pełną dobową akwizycję z zapasem, ale wszystko starsze jest
odrzucane w miarę napływu nowych danych i nie da się tego odzyskać. Nic nie jest
zapisywane na dysk.

**Wykresy nie są rejestratorem.** Dane wykresów żyją wyłącznie w pamięci i
znikają po zamknięciu okna. Do wszystkiego, co chcesz zachować, użyj **Start
logging** (zakładka Raw monitor): zapisuje każdą odebraną linię do pliku
`gpsdo_RRRR-MM-DD_GG-MM-SS.log` obok skryptu, z buforowaniem liniowym, więc
przebieg zakończony awarią i tak zostawia użyteczne dane. Uwaga: zapisywany jest
*surowy tekst telemetrii*, nie serie z wykresów — do ADEV i długich porównań ze
wzorcem podaj ten plik do TimeLab lub podobnego.

**Generate tz_table.h** przebudowuje tablicę stref czasowych firmware'u z danych
IANA na tej maszynie i zapisuje `tz_table.h` obok skryptu. Zastępuje dawny
`gen_tz_table.py`, więc tuner jest teraz jedynym skryptem do utrzymania.

Dane stref pochodzą z bazy systemowej na Linuksie/macOS albo z pakietu `tzdata`
Pythona — i tak to działa na Windows, gdzie system nie dostarcza żadnej bazy
IANA. Jeśli przycisk zgłosi brak danych, uruchom `pip install tzdata`; aby je
później odświeżyć, `pip install -U tzdata`. Wygenerowany nagłówek zapisuje, z
którego wydania IANA pochodzi, o ile da się to ustalić.

> Sama IANA publikuje *źródła* wymagające kompilatora `zic`, więc pobieranie
> bezpośrednio od nich nic by nie dało — pakiet `tzdata` to te same dane już
> skompilowane.

**Okno Raw monitor trzyma tylko ostatnie ~2500 linii** (niecałe pięć minut przy
tempie telemetrii). To ograniczenie wyświetlania, nie zapisu: po włączeniu
logowania plik dostaje wszystko, niezależnie od tego, co okno jeszcze pokazuje.

**Rozdzielczość to tempo telemetrii.** Jedna próbka na sekundę, więc cokolwiek
szybszego niż około 2 s jest niewidoczne: szybki cykl graniczny albo jitter na
pojedynczych pulsach się nie pojawi, a to, co widzisz, zostało już uśrednione
wewnątrz firmware.

**Jedno połączenie naraz.** Port szeregowy jest na wyłączność. Zamknij najpierw
inny terminal na tym samym porcie i pamiętaj, że tuner trzyma go, dopóki jest
otwarty.

**Wykresy ufają płytce.** Wartości są parsowane z tekstu telemetrii tak, jak
przyszły. Jeśli firmware zgłosi nieaktualną lub błędną liczbę, tuner narysuje ją
wiernie — niczego nie weryfikuje krzyżowo.

**Zapisy nie są trwałe.** Ustawienie parametru zmienia go tylko w RAM.
Parametry strojenia pętli wymagają jawnego `ES` (odpowiedź podaje dokładną
komendę); preferencje zapisują się same i mówią o tym.

---

*Część GPSDO FreeRTOS — [instrukcja firmware](README_PL.md) · [changelog](CHANGELOG_PL.md) · [repozytorium](https://github.com/jmnlabs/GPSDO_FreeRTOS)*
