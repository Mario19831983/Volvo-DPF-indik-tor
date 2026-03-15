# Volvo-DPF-indikátor
Návod jak si vyrobit vlastní DPF indikátor pro vozy Volvo. 2009 - 2016 přes ESP32 super mini

DPF indikátor funguje tak, že čte teplotu DPF a saze.

V hlavním menu je možnost nastavení LED diody. Pro saze bliká a pro teplotu svítí. 

Na mém Volvu XC60 rv2010 se spustilo vypalování DPF filtru při cca 28,5g. Teplota vzorsta ze 180°C na 230-240° a začlo vypalování. Cca za 10 minut jízdy saze spadly na 0.0g a v té chvíly se teplota DPF začala snižovat na 180°C cca asi 2 minuty.
Můžu si nastavit saze na 28g a v té chvíli začne led blikat. Znamená to pro mě, že se blíží vypalování.
Když nastavít teplotu na 210 tak při této teplote už startuje vypalování. Led kontrokla svítí dokud teplota neklesne pod 190°C
Vysledoval jsem, že každý den cestou do práce a z práce mi to udělá 1g sazí při 60 ujetých kilometrech.



Co budeme potřebovat.
- Software
1. Arduino IDE a nainstalované potřebné knihovny.

- Hardware
1. ESP32 super mini
2. 
