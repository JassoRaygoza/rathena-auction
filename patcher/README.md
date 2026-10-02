# rAthena Client Patcher

Windows tool that adds four [rAthena](https://github.com/rathena/rathena) client systems to a
Ragnarok Online client EXE:

| System | Server side |
| --- | --- |
| Change Material | [rathena-change-material](https://github.com/JassoRaygoza/rathena-change-material) |
| Rune Tablet | [rathena-rune-tablet](https://github.com/JassoRaygoza/rathena-rune-tablet) |
| Stylist UI | [rathena-stylist-ui](https://github.com/JassoRaygoza/rathena-stylist-ui) |
| Auction | [rathena-auction](https://github.com/JassoRaygoza/rathena-auction) |

Each system finds the code it needs by signature, so it works on any client build where those
signatures are found, not only on one exact file. If something does not match, the system is
shown as not compatible and nothing is written. If the EXE has no room for a new section header,
the code is appended to its last section instead (the 2025 client needs this).

**Download:** [release/ClientPatcher.exe](ClientPatcher.exe). It needs .NET Framework 4,
which is already part of Windows 10 and 11.

[Español](#uso-en-español)

## Supported clients

| System | 2026-02-11 (Ragexe 2026-02-19) | 2025-07-16 |
| --- | --- | --- |
| Change Material | Byte-identical to the original patcher | Signatures verified |
| Rune Tablet | Byte-identical to the original patcher | Signatures verified |
| Stylist UI | Byte-identical to the original patcher | Signatures verified |
| Auction | Byte-identical to the reference patcher | Signatures verified |

Other builds work when every signature matches; otherwise the tool refuses and writes nothing.
Test every system in game on a staging server before you distribute a patched client.

## Use

1. Run `ClientPatcher.exe`, press **Select EXE…** and choose the client EXE.
2. Systems that are ready to apply are checked. Grey systems show why they cannot be applied.
3. Press **Apply**. The tool writes `<name>_patched.exe` (and a `.report.json`) in the output
   folder. The original EXE is never modified.
4. **Diagnostics…** saves the address found for every signature. Include that file when you
   report that a build is not compatible.

The language can be switched between English and Spanish at the top right.

### Resources GRF

Stylist UI and Auction also need resources (textures and the Auction skin). They are written to a
resources GRF next to the patched EXE, `<name>_patched.grf`; it is created only when a selected
system has resources. The client must load it: tick **Add the resources GRF to the client's
DATA.INI** (the output folder must be the client folder, where DATA.INI is) or add the GRF to
DATA.INI yourself.

- The tool puts the GRF first (priority 0) and saves a `DATA.INI.<date>.bak` backup before
  changing it. Existing entries keep their exact bytes; Korean or accented names are not
  re-encoded.
- Because the GRF name goes into DATA.INI, the EXE name must be plain ASCII (no accents, `ñ` or
  Korean) for this option; otherwise rename the EXE.
- DATA.INI is checked before anything is written: it must exist, have a `[Data]` section, not be
  read-only or in use, and its folder must accept new files.
- Updating it is the last step of the write. If it fails, the patched EXE, its report and the GRF
  are removed, DATA.INI keeps its original bytes and no backup is left.

Apply all the systems you want in one run. Each run writes its own GRF (a run over
`X_patched.exe` writes `X_patched_patched.grf`), and the client must load every GRF written along
the way; `--data-ini` on each run does this, by hand they must all be listed.

### Command line

```
ClientPatcher.exe <exe> --systems changematerial,rune,stylist,auction [--out <folder>] [--data-ini]
ClientPatcher.exe --inspect <exe>
ClientPatcher.exe --resolve <exe> [--save <file.txt>]
```

ClientPatcher.exe is a window application, so wait for it explicitly: `start /wait
ClientPatcher.exe …` in cmd, or `Start-Process -Wait -NoNewWindow ClientPatcher.exe -ArgumentList
…` in PowerShell; then read `%ERRORLEVEL%` / `$LASTEXITCODE` (or `-PassThru` → `.ExitCode`).

- `--data-ini` updates DATA.INI as the checkbox does. The output folder must be the EXE's folder,
  DATA.INI must be there and usable, and the EXE name must be plain ASCII; otherwise the tool
  prints an error, exits 1 and writes nothing. If no GRF was produced, it prints a note and leaves
  DATA.INI alone.
- When a GRF is written, the output shows `Resources GRF: <path>`.
- Exit codes: 0 applied, 2 already applied, 1 error.

## Auction

What is patched:

- a new section `.auctn` with the Auction window code (from
  [rathena-auction](https://github.com/JassoRaygoza/rathena-auction), embedded in the tool);
- an import table inside it, filled with the addresses found in the client;
- the client's two font functions, hooked so the Auction window uses Arial 18 inside its own
  frames only (the rest of the client keeps its fonts);
- the packet dispatcher entries for 0x025F (open/close the window) and 0x0252 (Auction replies);
- the `auction_x2` skin (167 bitmaps), written to the resources GRF.

The patched client needs the server from
[rathena-auction](https://github.com/JassoRaygoza/rathena-auction):

- `feature.auction: 2` (with 2, only the modern Auction is accepted; the classic register and bid
  packets are ignored);
- `sql-files/upgrades/upgrade_20261001_auction.sql` imported into the database, also on fresh
  installs;
- optional, in `conf/battle/misc.conf`: `auction_feeperhour` (default 12000) and
  `auction_maximumprice` (default 500000000);
- the emulator built for the client's `PACKETVER`. Clients from 2025 onwards encrypt the opcode of
  every packet they send, so the server must also be built with the matching packet obfuscation
  keys for that client date.

## Build from source

Requires Visual Studio (for its Roslyn `csc.exe`). Users only need .NET Framework 4.x.

```
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 -Test
```

This builds `bin\ClientPatcher.exe` and runs the test suite. Tests that need real client EXEs read
their paths from `tests\fixtures.local.txt`; copy
[tests/fixtures.example.txt](https://github.com/JassoRaygoza/rathena-client-patcher/blob/main/tests/fixtures.example.txt) to that name and fill in the paths you
have. Tests whose EXE is not configured are skipped.

| Path | Contents |
| --- | --- |
| [src/Core/](https://github.com/JassoRaygoza/rathena-client-patcher/tree/main/src/Core) | PE reading and writing, signature resolver, patch engine, GRF and DATA.INI |
| [src/Modules/](https://github.com/JassoRaygoza/rathena-client-patcher/tree/main/src/Modules) | One module per system: signatures, state detection and edits |
| [src/App/](https://github.com/JassoRaygoza/rathena-client-patcher/tree/main/src/App) | Window, command line and EN/ES strings |
| [resources/](https://github.com/JassoRaygoza/rathena-client-patcher/tree/main/resources) | Embedded payloads: Stylist textures, Auction section image and skin |
| [tools/](https://github.com/JassoRaygoza/rathena-client-patcher/tree/main/tools) | Generators for the Stylist payload and the Auction artifacts |
| [tests/](https://github.com/JassoRaygoza/rathena-client-patcher/tree/main/tests) | Test suite (synthetic PE files plus the real client fixtures) |

## Uso en español

1. Abre `ClientPatcher.exe` (el idioma se cambia arriba a la derecha), pulsa **Elegir EXE…** y
   elige el EXE del cliente.
2. Los sistemas listos para aplicar aparecen marcados; los grises indican por qué no se pueden
   aplicar.
3. Pulsa **Aplicar**: se crea `<nombre>_patched.exe` y su `.report.json` en la carpeta de salida.
   El EXE original no se modifica.
4. **Diagnóstico…** guarda la dirección encontrada para cada firma; envíalo cuando un build no sea
   compatible.

Stylist UI y Subastas necesitan recursos (texturas y el skin de subastas), que se guardan en un
GRF junto al EXE parcheado (`<nombre>_patched.grf`; solo se crea si algún sistema elegido tiene
recursos). El cliente debe cargarlo: marca **Agregar el GRF de recursos al DATA.INI del cliente**
(la carpeta de salida debe ser la del cliente, donde está DATA.INI) o agrégalo tú al DATA.INI.

- La herramienta pone el GRF primero (prioridad 0) y guarda un respaldo `DATA.INI.<fecha>.bak`
  antes de cambiarlo. Las entradas existentes conservan sus bytes exactos.
- Para esta opción el nombre del EXE debe ser ASCII simple (sin acentos, `ñ` ni coreano); si no,
  renombra el EXE.
- DATA.INI se revisa antes de escribir nada. Actualizarlo es el último paso: si falla, se borran
  el EXE parcheado, su reporte y el GRF, y DATA.INI conserva sus bytes originales.

Aplica todos los sistemas que quieras en una sola pasada. Cada pasada escribe su propio GRF y el
cliente debe cargar todos los GRF escritos en el camino.

Subastas necesita el servidor de [rathena-auction](https://github.com/JassoRaygoza/rathena-auction)
con `feature.auction: 2`, el SQL `upgrade_20261001_auction.sql` importado (también en instalaciones
nuevas) y el emulador compilado para el `PACKETVER` del cliente. Los clientes 2025 cifran el
opcode de cada paquete, así que el servidor también necesita las claves de paquetes de ese cliente.

Prueba cada sistema en el juego, en un servidor de pruebas, antes de distribuir un cliente
parcheado.

## License

[GNU GPL v3](../LICENSE). Ragnarok Online client resources remain the property of their respective
owners and are provided only for use with your own client.
