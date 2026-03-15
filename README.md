# Volvo-DPF-indikátor
Návod jak si vyrobit vlastní DPF indikátor pro vozy Volvo. 2009 - 2016 přes ESP32 super mini

DPF indikátor funguje tak, že čte teplotu DPF a saze.

V hlavním menu je možnost nastavení LED diody. Pro saze bliká a pro teplotu svítí. 

Na mém Volvu XC60 rv2010 se spustilo vypalování DPF filtru při cca 28,5g. Teplota vzorsta ze 180°C na 230-240° a začlo vypalování. Cca za 10 minut jízdy saze spadly na 0.0g a v té chvíly se teplota DPF začala snižovat na 180°C cca asi 2 minuty.
Můžu si nastavit saze na 28g a v té chvíli začne led blikat. Znamená to pro mě, že se blíží vypalování.
Když nastavít teplotu na 210 tak při této teplote už startuje vypalování. Led kontrokla svítí dokud teplota neklesne pod 190°C
Vysledoval jsem, že každý den cestou do práce a z práce mi to udělá 1g sazí při 60 ujetých kilometrech.

První dvě orazovky jsou havní menu. Další dvě obrazovky je phone verze.
<img src="10.png" width="600">

Co budeme potřebovat.
- Software
1. Arduino IDE a nainstalované potřebné knihovny.

- Hardware
1. ESP32 super mini
2. Step down nastavitelný měnič s LM2596 DC-DC (Tady velký pozor. Nejdřív si nastavte na outputu 5V trimrem. Přijpoj si samotný měnič na zdroj 12V a nastav si 5.05V sleduj meřák na outputu. Jinak spálíš všechno co tam bude.)
3. MCP2515 CAN Bus Modul TJA1050 SPI
4. Diodu 1N4148
5. Led diodu. Barvu si zvol sám.
6. Odpory 10kOhm,18kOhm pro dělič napětí a 320Ohm pro ledku.
7. Univerzální plošný spoj 50x70mm
8. Kabel 3x1mm pro GND, CANH, CANL který se napojí zezadu na konektor OBD 2. Vodič 1mm pro 12V ze zapalovače.
9. Pojitku 500mA a pojistkové lůžko.
10. OBD konektor když chceš verzi plug and play.

Schéma zapojení.

Pro zkoužku si můžeš půjčit 12V přímo z OBD2 na pinu 16, ale protože je tam trvalé napětí, tak je lepší to zapojit na 12V ze zapalovače. Ten se zapne až po nastartování.
<img src="schema.png" width="600">

Jak to vypadá u mě. Ten microUSB je tam jen kabel, kdybych chtěl software aktualizovat.

<img src="bastl.png" width="600">

Nahrání do ESP32.
1. jako board si nastav NOLOGO ESP32C3 Super mini
2. stáhni si knihovnu ACAN2515.h od Pierre Molinaro
3. po vložení INA můžeš nahrávat.
4. Pokud špatně zapojíš MCP2515 CAN Bus Modul TJA1050 SPI esp32 vůbec nenaběhne.
5. Když je vše OK tak se připoj na wifi MY_VOLVO a heslo je 12345678

## Demo version

Free demo firmware:

[Download demo .ino](demo.ino)
