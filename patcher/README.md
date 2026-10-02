# Client Patcher

Applies the server's client systems to a Ragnarok client EXE. Each system finds the code it
needs by signature, so it works on any client build where those signatures are found, not
only on one exact file. If something does not match, the system is shown as not compatible
and nothing is written. If the EXE has no room for a new section header, the code is appended to its last section instead (the 2025 client needs this).

| System | 2026-02-11 (Ragexe 2026-02-19) | 2025-07-16 |
|---|---|---|
| Change Material | Byte-identical to the original patcher | Signatures verified; in-game check pending |
| Rune Tablet | Byte-identical to the original patcher | Signatures verified; in-game check pending |
| Stylist UI | Byte-identical to StylistUIPatcher | Signatures verified; in-game check pending |
| Auction | Equivalent to auction_exe_patch.py (same table, same constants) | Signatures verified; in-game check pending |

Check docs/stage1-ingame-checklist.md (it covers all four systems) in game before relying on a build marked "in-game check pending".

## Use

1. Run `ClientPatcher.exe`, press **Select EXE…** and choose the client EXE.
2. Systems that are ready to apply are checked. Grey systems show why they cannot be applied.
3. Press **Apply**. The tool writes `<name>_patched.exe` (and a `.report.json`) in the output
   folder. The original EXE is never modified.
4. **Diagnostics…** saves the address found for every signature; send it when a build is not
   compatible.

Stylist UI and Auction also need resources (textures and the Auction skin). They are written to a resources GRF next to the patched EXE,
`<name>_patched.grf`; it is created only when a selected system has resources. The client must
load it: tick **Add the resources GRF to the client's DATA.INI** (the output folder must be the
client folder, where DATA.INI is) or add the GRF to DATA.INI yourself. The tool puts the GRF first
(priority 0) and saves a `DATA.INI.<date>.bak` backup before changing it. Existing entries keep
their exact bytes (Korean or accented names are not re-encoded). Because the GRF name
goes into DATA.INI, the EXE name must be plain ASCII (no accents, `ñ` or Korean) for this option;
otherwise rename the EXE. DATA.INI is checked before anything is written (it must exist, have a
`[Data]` section, not be read-only or in use, and its folder must accept new files). Updating it is
the last step of the write: if it fails, the patched EXE, its report and the GRF are removed,
DATA.INI keeps its original bytes and no backup is left.

Apply all the systems you want in one run. Each run writes its own GRF (a run over
`X_patched.exe` writes `X_patched_patched.grf`), and the client must load every GRF written
along the way; `--data-ini` on each run does this, by hand they must all be listed.

Command line. ClientPatcher.exe is a window application, so wait for it explicitly:
`start /wait ClientPatcher.exe …` in cmd, or
`Start-Process -Wait -NoNewWindow ClientPatcher.exe -ArgumentList …` in PowerShell; then read
`%ERRORLEVEL%` / `$LASTEXITCODE` (or `-PassThru` → `.ExitCode`).

```
ClientPatcher.exe <exe> --systems changematerial,rune,stylist,auction [--out <folder>] [--data-ini]
ClientPatcher.exe --inspect <exe>
ClientPatcher.exe --resolve <exe> [--save <file.txt>]
```

`--data-ini` updates DATA.INI as the checkbox does. The output folder must be the EXE's folder,
DATA.INI must be there and usable, and the EXE name must be plain ASCII; otherwise the tool prints
an error, exits 1 and writes nothing. If the DATA.INI update itself fails, everything written is
removed and the tool exits 1. If no GRF was produced, it prints a note and leaves DATA.INI alone.
When a GRF is written, the output shows `Resources GRF: <path>`.

Exit codes: 0 applied, 2 already applied, 1 error.

## Auction

Adds the Auction window to the client. What is patched:

- a new section `.auctn` with the Auction window code (built from rathena-auction-client and embedded in the tool);
- an import table inside it, filled with the addresses found in the client;
- the client's two font functions, hooked so the Auction window uses Arial 18 inside its own windows only (the rest of the client keeps its fonts);
- the packet dispatcher entries for 0x025F (open/close the window) and 0x0252 (listing response, AUC2);
- the `auction_x2` skin (167 bitmaps), written to the resources GRF `<name>_patched.grf`. Load that GRF as described above (`--data-ini` or by hand), or the window has no skin.

Supported builds: the 2026-02-11 / 2026-02-19 family and the 2025-07-16 build. Other builds work if every signature matches; otherwise the tool refuses and writes nothing.

Server requirement. The patched client only works against the rAthena emulator from the rathena-auction repository, `auction` branch:

- `feature.auction: 2` in `conf/battle/feature.conf` (with 2, only the modern Auction is accepted; the classic register/bid packets are ignored);
- import `sql-files/upgrades/upgrade_20261001_auction.sql` into the server database (back it up first and stop map-server and char-server). A fresh install needs it too: `main.sql` only adds the `bound` column and does not create the `auction_settlement` and `auction_operation` tables. Without it, the listing and the actions fail on the server;
- optional, in `conf/battle/misc.conf`: `auction_feeperhour` (default 12000; charged when an item is listed, fee × duration of 1 to 48 hours) and `auction_maximumprice` (default 500000000; upper limit for the buy-now price, bids must stay below it);
- the emulator must be built for the client's `PACKETVER`;
- 2025 clients use packet obfuscation, so the server must be built with the packet keys that match that client date;
- opcodes involved: 0x0251 (request), 0x0252 (response) and 0x025F (open window).

## Uso (español)

1. Abre `ClientPatcher.exe` (puedes cambiar el idioma arriba a la derecha), pulsa
   **Elegir EXE…** y elige el EXE del cliente.
2. Los sistemas listos para aplicar aparecen marcados; los grises indican por qué no se pueden
   aplicar.
3. Pulsa **Aplicar**: se crea `<nombre>_patched.exe` y su `.report.json` en la carpeta de salida.
   El EXE original no se modifica.
4. **Diagnóstico…** guarda la dirección encontrada para cada firma; envíalo cuando un build no
   sea compatible.

Stylist UI necesita dos texturas, que se guardan en un GRF de recursos junto al EXE parcheado
(`<nombre>_patched.grf`; solo se crea si algún sistema elegido tiene recursos). El cliente debe
cargarlo: marca **Agregar el GRF de recursos al DATA.INI del cliente** (la carpeta de salida debe
ser la del cliente, donde está DATA.INI) o agrégalo tú al DATA.INI. La herramienta pone el GRF
primero (prioridad 0) y guarda un respaldo `DATA.INI.<fecha>.bak` antes de cambiarlo. Las entradas
existentes conservan sus bytes exactos (los nombres en coreano o con acentos no se recodifican). Como el
nombre del GRF se escribe en DATA.INI, para esta opción el nombre del EXE debe ser ASCII simple
(sin acentos, `ñ` ni coreano); si no, renombra el EXE. DATA.INI se revisa antes de escribir nada
(debe existir, tener la sección `[Data]`, no ser de solo lectura ni estar en uso, y su carpeta debe
admitir archivos nuevos). Actualizarlo es el último paso: si falla, se borran el EXE parcheado, su
reporte y el GRF, DATA.INI conserva sus bytes originales y no queda respaldo. En la línea de
comandos usa `--data-ini` (mismas reglas; si algo falla, da error, sale con código 1 y no deja
nada escrito).

Cuando se escribe un GRF, la salida muestra `Resources GRF: <ruta>`.

Aplica todos los sistemas que quieras en una sola pasada. Cada pasada escribe su propio GRF
(una pasada sobre `X_patched.exe` escribe `X_patched_patched.grf`) y el cliente debe cargar todos
los GRF escritos en el camino; `--data-ini` en cada pasada lo hace, a mano hay que listarlos todos.

En 2025 las firmas están verificadas; falta la prueba en juego (docs/stage1-ingame-checklist.md).

## Subastas (Auction)

Agrega la ventana de subastas al cliente. Qué se parchea:

- una sección nueva `.auctn` con el código de la ventana (compilado desde rathena-auction-client e incluido en la herramienta);
- una tabla de imports dentro de ella, rellenada con las direcciones encontradas en el cliente;
- las dos funciones de fuente del cliente, enganchadas para que la ventana use Arial 18 solo dentro de sus propias ventanas (el resto del cliente conserva sus fuentes);
- las entradas del despachador de paquetes para 0x025F (abrir/cerrar la ventana) y 0x0252 (respuesta del listado, AUC2);
- el skin `auction_x2` (167 bitmaps), escrito en el GRF de recursos `<nombre>_patched.grf`. Carga ese GRF como se explicó arriba (`--data-ini` o a mano); sin él la ventana no tiene skin.

Builds soportados: la familia 2026-02-11 / 2026-02-19 y el build 2025-07-16. Otros builds funcionan si todas las firmas coinciden; si no, la herramienta se niega y no escribe nada.

Requisito del servidor. El cliente parcheado solo funciona con el emulador rAthena del repositorio rathena-auction, rama `auction`:

- `feature.auction: 2` en `conf/battle/feature.conf` (con 2 solo se acepta la subasta moderna; los paquetes clásicos de registro/puja se ignoran);
- importa `sql-files/upgrades/upgrade_20261001_auction.sql` en la base de datos del servidor (haz un respaldo antes y detén map-server y char-server). También hace falta en una instalación nueva: `main.sql` solo agrega la columna `bound` y no crea las tablas `auction_settlement` y `auction_operation`. Sin esta actualización, el listado y las acciones fallan en el servidor;
- opcional, en `conf/battle/misc.conf`: `auction_feeperhour` (por defecto 12000; se cobra al publicar un objeto, tarifa × duración de 1 a 48 horas) y `auction_maximumprice` (por defecto 500000000; límite del precio de compra inmediata, las pujas deben quedar por debajo);
- el emulador debe compilarse para el `PACKETVER` del cliente;
- los clientes 2025 usan ofuscación de paquetes, así que el servidor debe compilarse con las claves de paquetes que correspondan a la fecha de ese cliente;
- opcodes involucrados: 0x0251 (petición), 0x0252 (respuesta) y 0x025F (abrir ventana).

## Build

Requires Visual Studio (for its Roslyn `csc.exe`). Users only need .NET Framework 4.x.

```
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 -Test
```

`tests/fixtures.local.txt` lists real client EXEs used by the tests (local paths only).
