# Protocol of the Brilliant 98770/05 remote

Everything here was found by listening to the original remote with an nRF24L01+ and by trial and
error against real lamps (two lamps, November 2026). Values that were not verified are marked.

## Hardware

- Remote: Brilliant 98770/05, PCB marked LAMDEN "CCT+RGB-31Key" (2020-03-26).
- SoC: Powerlink **PL51WT020** (8051 core with a 2.4 GHz transceiver). Its RF block is driven over
  an internal SPI interface with 16-bit registers and PKT/FIFO flags, i.e. the PL1167/LT8900
  family. The datasheet lists **1 Mbit/s only**. The firmware is read-protected.
- The lamps contain the same kind of radio. They only receive; they never acknowledge.

## Radio layer

| | |
|---|---|
| Frequency | 2450 MHz (nRF24 channel 50). The 98770/05 used only this channel. |
| Rate | 1 Mbit/s GFSK |
| Preamble | `AA` repeated |
| Sync word | `8C 42 F3 7B` as read MSB first by an nRF24 |
| Trailer | 4 bits `0101` |
| Byte order | after the trailer every byte is sent **LSB first** |

An nRF24 can receive this with the 3-byte address `8C 42 F3`, CRC disabled and a fixed payload of
32 bytes. The payload then starts with `7B`, the trailer and the frame. To transmit, use the
5-byte address `AA AA 8C 42 F3` (two preamble bytes in front) and put `7B`, `0101` and the
bit-reversed frame into the payload.

## Frame

17 bytes after the trailer (each LSB first on air):

| Byte | Content |
|---|---|
| 0 | Length `0E` (14 data bytes) |
| 1-5 | Remote id, e.g. `10 00 xx xx 31`. Every remote has its own; a lamp only obeys its paired remote. |
| 6 | `00` |
| 7 | Rolling counter |
| 8 | Command |
| 9-12 | Parameters p1..p4 |
| 13 | `01` or `02`, see below |
| 14 | Checksum: sum of bytes 1..12 modulo 256 (byte 13 is **not** included) |
| 15-16 | CRC-16, low byte first |

CRC: polynomial 0x1021 reflected (0x8408), initial value 0, no final XOR, over bytes 0..14
(length byte included). This is the PL1167 hardware CRC.

## Commands

| Key | Command | Parameters |
|---|---|---|
| ON | `01` | - |
| OFF | `09` | - |
| + | `04` | p1 = step (`0A`) |
| - | `05` | p1 = step (`05`) |
| Colder (snowflake) | `0C` | - |
| Warmer (sun) | `1C` | - |
| Centre (light/moon) | `14` | - |
| CC / C/W / WW | `12` / `1A` / `03` | p1 brightness %, p2 warm %, p3 cold % |
| Red / Green / Yellow / Blue | `11` / `19` / `02` / `0A` | p1 brightness %, p2..p4 red/green/blue % |
| Colour cycle (RGB circle) | `15` | as above, the remote steps through colours |
| RGB brightness (RGB triangle) | `13` | p1 brightness % (70/50/35/25/15) |
| Effect (RGB play/pause) | `0D` | - |
| RGB OFF | `1B` | - |

Verified details:

- **White** (`12`) accepts free values: any brightness, and warm + cold = 100. Brightness 0 turns
  off only the white part; the RGB ring stays on.
- **Colour** (`11`): p2..p4 must **add up to 100**; other sums are ignored. p1 is the
  brightness - the colour keys send 100. Sending colour and brightness in one command avoids the
  ring flashing at 100 % that you get with `11` followed by `13`.
- Below about 15 % the RGB ring stays dark.
- White at 1 % flickers slightly on our lamp, 2 % does not.
- `01` (ON) always switches the white part back on and restores the ring.
- The remote also has group keys 1-4 (not analysed yet).

## Rolling counter

Byte 7 increases by one per key press; all copies of one press carry the same value. Observed rules
of the lamp:

- A counter value that was used recently is **rejected** (replay protection). It is probably not
  just a comparison with the last value: re-using values from a few packets earlier failed
  repeatedly. (Those tests overlapped with the byte 13 issue below, so the exact history length is
  unknown - the safe rule is to never re-use a value.)
- Big jumps **forward** are accepted (+64 tested).
- Going **back** works for about 40 steps; 64 and 100 back were rejected.
- There is **no wrap-around**: after `FF` the lamp rejects `00`, `01`, ... So the original remote is
  locked out once it wraps, until its counter is within reach again. Whether cutting the power
  resets the lamp's counter memory is not verified.

What the component does with this:

- It counts +1 per packet and never re-uses a value.
- It stays ahead of the remote. When it hears the remote with a counter that is behind, it repeats
  the same command once with a fresh counter. For the user the remote keeps working.
- Before reaching `F0` it steps the lamp down in steps of 32, re-sending the current state each time.

## Byte 13

- Colour commands (`11`, `15`, `19`, `0A`, `02`) always use `02`, and the lamp only accepts them
  with `02`.
- For the other commands (`01`, `09`, `12`, `1B`, `04`, `05`, ...) the remote sometimes sent `02`
  and sometimes `01`. The lamp accepted the same white command only with `01` at one time and only
  with `02` after it had been switched off and on. We could not find out what decides it.
- The component therefore sends these commands twice, once with each value. The wrong one is
  ignored; because the commands set absolute values, applying one twice does no harm.

## How the protocol was found

This may help with other unknown 2.4 GHz remotes.

1. Open the remote and read the chip (here PL51WT020 → PL1167 family, 1 Mbit/s).
2. nRF24 in receive mode, 2-byte address, CRC off. Use addresses that match an idle carrier
   followed by one preamble byte (`00 AA`, `00 55`, `FF AA`, `FF 55` in both byte orders) and
   `AA AA`. Scan channels and log every packet.
3. Press a key **repeatedly** (a held ON key sends only one short burst; dimming keys repeat).
   Compare packets during key presses with packets in pauses and look for bit sequences that only
   show up during presses. WLAN beacons also produce repeating packets; they appear in pauses too.
4. Once a candidate sync word shows up, listen with it as a fixed address and capture each key.
5. Brute-force the CRC parameters on captured frames.
