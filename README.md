# rAthena Auction

Modern **Auction** system for [rAthena](https://github.com/rathena/rathena) Renewal: a complete
server DIFF, a native Auction window for current clients, its skin, and a Windows client EXE
patcher that adds the window to any compatible client.

The classic Auction was removed from clients in 2014. This package brings it back as a new window
that talks to the server through a dedicated, versioned protocol. Every sale, bid and purchase is
stored durably, so no item or zeny is lost if a server stops in the middle of a trade.

[Requirements](#requirements) | [Quick install](#quick-install) | [Configuration](#configuration) | [Client patching](#client-patching) | [Patcher guide](patcher/README.md)

## Features

- Live catalog with category tabs (Armor, Weapon, Card, Misc, Costume, All), name search and
  price search, with paging.
- **My sales** and **My bids** lists, plus an inventory view for choosing what to sell.
- Register a sale with a starting bid, an optional buy-now price and a duration of 1 to 48 hours.
- Bid, buy now, cancel a sale or close it early to the highest bidder.
- Items and zeny are delivered by mail when a sale ends, is cancelled, is closed or expires.
- Durable trading: purchases go through an operation journal with a request hash, so a retried
  or interrupted request is applied exactly once. Zeny and items reserved for a pending purchase
  cannot be spent, merged or dropped by logging out until the server answers.
- Server-side limits that the window shows to the player: listing fee per hour, maximum price,
  minimum and maximum hours, and at most 5 sales and 5 bids per character.
- Refine, enchant grade, cards and random options are shown for every listed item.
- The window uses Arial 18 inside its own frames only; the rest of the client keeps its fonts.

## Requirements

- rAthena Renewal. Tested base: `e985006171d2eb320ee512a653f4c83aea3d81b6`.
- MySQL or MariaDB. The SQL upgrade converts `auction`, `mail`, `mail_attachments`, `char` and
  `inventory` to InnoDB, because trades run inside transactions.
- A supported client EXE:
  - the **2026-02-11 / 2026-02-19** client family;
  - the **2025-07-16** client.
  Other builds work when every signature the patcher checks matches. The patcher refuses any EXE
  it cannot verify; it never patches blindly.
- The emulator built for your client's `PACKETVER`. Clients from 2025 onwards encrypt the opcode
  of every packet they send, so the server must also be built with the matching packet
  obfuscation keys for that client date.
- Windows with .NET Framework 4 for the patcher. It is already part of Windows 10 and 11.

## Package layout

| Path | Contents |
| --- | --- |
| [diff/auction.diff](diff/auction.diff) | Complete server patch, including modified and new files. **Apply this file.** |
| [server/](server/) | The seven new server files already included in the DIFF, for browsing and review |
| [patcher/ClientPatcher.exe](patcher/ClientPatcher.exe) | Client EXE patcher with the Auction window and its skin built in ([guide](patcher/README.md)) |
| [client/resources/](client/resources/) | The `auction_x2` skin: 167 bitmaps (the Auction skin at 2× scale plus every button state), and the same files packed as [auction_x2.grf](client/resources/auction_x2.grf) |
| [client/source/](client/source/) | Source of the Auction window and its build scripts (for developers) |

## Quick install

1. Back up your database. From the rAthena root, apply the server patch:

   ```sh
   git apply --check "/path/to/rathena-auction/diff/auction.diff"
   git apply "/path/to/rathena-auction/diff/auction.diff"
   ```

2. With map-server and char-server stopped, import the SQL upgrade. Fresh installs need it too:

   ```sh
   mysql -u <user> -p <database> < sql-files/upgrades/upgrade_20261001_auction.sql
   ```

3. Enable the system in `conf/import/battle_conf.txt`:

   ```
   feature.auction: 2
   ```

4. Rebuild **both** map-server and char-server. They share the Auction protocol and must come
   from the same build.

5. Patch the client EXE with [ClientPatcher.exe](patcher/ClientPatcher.exe) and load the resources
   GRF it writes. See [Client patching](#client-patching).

6. Open the window in game with `@auction`, or from an NPC with the `openauction;` script command.

## Configuration

| Setting | File | Default | Purpose |
| --- | --- | --- | --- |
| `feature.auction` | `conf/battle/feature.conf` | `off` | `0` off, `1` classic Auction (disabled on 2014+ clients), `2` modern Auction for any client version |
| `auction_feeperhour` | `conf/battle/misc.conf` | `12000` | Fee charged when an item is listed: fee × duration in hours |
| `auction_maximumprice` | `conf/battle/misc.conf` | `500000000` | Upper limit for the buy-now price; bids must stay below it |

Later settings in `conf/import/battle_conf.txt` take precedence. With `feature.auction: 2`, only the
modern protocol is accepted: the classic register and bid packets are ignored.

## Client patching

1. Run [ClientPatcher.exe](patcher/ClientPatcher.exe) and press **Select EXE…**.
2. Tick **Auction**. You can also tick Change Material, Rune Tablet and Stylist UI. Apply every
   system you want in one run: each run writes its own resources GRF.
3. Choose the client folder as the output folder and tick
   **Add the resources GRF to the client's DATA.INI**. The patcher puts the GRF first in DATA.INI
   and keeps a backup. Without that GRF, the window has no skin.
4. Press **Apply**. The patcher writes `<name>_patched.exe`, `<name>_patched.grf` and a report.
   The original EXE is never modified.

The skin is also available on its own in [client/resources/](client/resources/), as loose files
under `data\texture\` or as [auction_x2.grf](client/resources/auction_x2.grf), for servers that
pack their own GRF.

What the patcher changes for Auction:

- adds a new `.auctn` section with the window code and fills its import table with the
  addresses found in your client;
- hooks the two client font functions, for the Auction window's own frames only;
- routes packet `0x025F` (open window) and `0x0252` (Auction replies) to the new window;
- writes the `auction_x2` skin (167 bitmaps) to the resources GRF.

**Diagnostics…** saves every address the patcher found. Include that file when you report that a
build is not compatible.

## Protocol

| Opcode | Direction | Use |
| --- | --- | --- |
| `0x025F` | server → client | Opens or closes the Auction window |
| `0x0251` | client → server | Catalog queries and actions (`type \| 0x8000`) |
| `0x0252` | server → client | Versioned replies: listings, rules and action results |

The wire format is defined in
[auction_protocol.hpp](server/src/common/auction_protocol.hpp). The same header is compiled into
the server and into the client window.

## Validation status

- The DIFF applies cleanly to the tested base and reproduces every changed and new server file.
- map-server and char-server build with Visual Studio 18 (x64).
- The patcher passes 157 automated tests. Among them:
  - its output on the 2026 client is byte-identical to the reference patcher;
  - all four systems apply to the 2025 client.

Test in game on a staging server before you use it in production: open the window, list, bid,
buy, cancel and close sales, and check that chat, movement and NPC dialogs still work after you
have used the window.

## Building the window (developers)

The window is C++ built without a runtime into an import-free image (`auction_section.vcxproj`,
Visual Studio, Win32). Every client address goes through the exported `auction_imports` table, so
the same image works on every supported client. `client/source/build/build.ps1` builds the image,
scales the skin and produces a reference patched EXE. It expects this repository's server tree for
`auction_protocol.hpp`.

## Credits and license

Server code derives from rAthena and is distributed under the [GNU GPL v3](LICENSE).
Ragnarok Online client resources, including the original Auction skin bitmaps, remain the
property of their respective owners and are provided only for use with your own client.
