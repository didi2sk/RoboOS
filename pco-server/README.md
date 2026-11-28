# Robo PCO

Linuxový prijímač pre integráciu s alarmom RoboOS. Ukladá Contact ID udalosti do SQLite a zobrazuje dispečing s históriou a otvorenými incidentmi. Stav dispečingu sa obnovuje každých osem sekúnd, keď je stránka aktívna.

## Spustenie na Linuxe

Použi Python 3.11 alebo novší a systémové dáta časových pásiem (balík `tzdata` na Debiane a Ubuntu). Aplikácia používa knižnice dodávané s Pythonom.

```bash
cd pco-server
read -rsp 'Heslo dispečingu (min. 12 znakov): ' PCO_PASSWORD
echo
export PCO_PASSWORD
python3 pco.py
```

Otvor **http://127.0.0.1:8080/** a prihlás sa menom **operator** a zadaným heslom. Meno zmeníš cez `PCO_OPERATOR`. Pre prístup k vzdialenému serveru použi napríklad `ssh -L 8080:127.0.0.1:8080 používateľ@server`. Web počúva len na loopbacku. Použi `--web-port`, ak je port obsadený.

Pre RoboOS nastav `--listen <privátna-IP-servera>`. Predvolený port je **TCP 5000**. Použi privátny segment alebo WireGuard medzi lokalitami aj pri LTE zálohe. Pilotné správy sú v otvorenom texte a číslo objektu slúži na jeho identifikáciu. Prijímač sprístupni iba zariadeniam oprávneným posielať udalosti.

`objects.json` obsahuje účty objektov, názvy zón a čas dohľadu v sekundách. Nula vypne dohľad. Položka zóny môže byť `003` pre všetky skupiny alebo `01/003` pre konkrétnu skupinu. Pri štarte sa vytvorí súbor `pco.sqlite3`. Cesty nastavíš cez `--config` a `--db`.

## Spustenie na Windows

Otvor PowerShell v priečinku `pco-server`.

```powershell
py -m pip install tzdata
$env:PCO_PASSWORD = Read-Host "Zadaj heslo dispecingu (aspon 12 znakov)"
py .\pco.py
```

## Dohoda pre vysielač RoboOS

| Parameter | Pilot |
|---|---|
| Prijímač | Privátna IP Linux servera, TCP 5000 |
| Profil | DC‑09 `ADM-CID` pre udalosti, `NULL` pre dohľad |
| Účet a prefix | `1001`, `L0` |
| Dohľad | `NULL` každých 30 sekúnd, výpadok po 90 sekundách bez platnej správy |
| Poplach zadných dverí | `1130 01 003` |
| Návrat zóny po poplachu | `3130 01 003` |
| Zastráženie používateľom 001 | `3401 01 001` |
| Odstráženie používateľom 001 | `1401 01 001` |
| Pravidelný test prenosu | `1602 01 000` |

Názvy v dispečingu: 130 poplach (24h zóna 133 = „Poplach 24h“) so zónou, 120 panika, 3130 / 3120 návrat, 3401 / 1401
zastrážené / odstrážené, 3441 / 1441 zastrážené doma / odstrážené (doma), 406 poplach zrušený (všetky s číslom
užívateľa: 001 = kód, 011.. = karty RoboOS), 461 zablokovaný kód (otvorí incident), 305 zapnutie / reštart ústredne,
301 výpadok / obnova napájania, 602 test prenosu. Ostatné kódy sa uložia ako „Udalosť Contact ID“.

Príklad nastavenia v konzole RoboOS. Adresu nahraď privátnou IP servera.

```text
pco host 192.168.1.50 5000 tcp
pco account 1001
pco receiver off
pco line 0
pco format cid
pco key off
pco heartbeat 30
pco on
pco test
pco send E130 3
```

Po `pco test` sa v histórii objaví test prenosu. Príkaz `pco send E130 3` vytvorí skúšobný incident pre zadné dvere.

Rámec poplachu a očakávaná odpoveď. Zápisy `\n` a `\r` predstavujú jednotlivé riadiace bajty LF a CR.

```text
RoboOS → PCO   \n04700027"ADM-CID"0001L0#1001[#1001|1130 01 003]\r
PCO → RoboOS   \n9EA80012"ACK"0001L0#1001[]\r
RoboOS → PCO   \n9B9B0013"NULL"0000L0#1001[]\r
```

Pri opakovaní zachovaj sekvenciu a dátovú časť vrátane prípadného časového údaja. PCO potvrdí identickú udalosť na tom istom účte a sekvencii aj po zmene TCP spojenia. Deduplikačné okno je 180 sekúnd od prvého uloženia. Prijímač overuje CRC a dĺžku rámca, známy účet a formát udalosti. Má podporu viacerých rámcov v jednom TCP spojení. `ACK` odošle až po potvrdení databázového zápisu. Pri neznámom účte alebo nesprávnom CRC odpoveď nepošle. Pri známom účte a nepodporovanom type vráti `DUH`.

Kladné potvrdenie protokolu a prevzatie operátorom majú samostatný význam. Návrat zóny do normálu sa uloží do histórie a incident zostáva otvorený na spracovanie. Audit ukladá čas, meno operátora a vykonaný krok.

## Skúška

Spusti z koreňa repozitára. Na Windows použi `py .\pco-server\smoke.py`.

```bash
python3 pco-server/smoke.py
```

Skúška spúšťa dočasný TCP prijímač a web, overuje publikovaný príklad DC‑09, rozdelené a spojené rámce, poškodené CRC, zopakovanie poplachu, odstraženie a zastráženie, dohľad, incidenty, audit a opätovné otvorenie databázy.

Toto vydanie je určené na pilotnú integráciu. Má TCP profil `ADM-CID` a `NULL` bez šifrovania na úrovni DC‑09, jednu inštanciu a jeden účet operátora. Overenie s fyzickým RoboOS a prepnutie LAN/LTE sa robí v cieľovej sieti. Pri zálohe cez router sleduje PCO dostupnosť objektu ako celku.

Odkazy: [RoboOS alarm](https://github.com/didi2sk/RoboOS/blob/main/src/alarm.c), [SIA DC‑09](https://www.securityindustry.org/industry-standards/dc-09-2026/), [Ademco Contact ID](https://www.securityindustry.org/industry-standards/dc-05-2016/).
