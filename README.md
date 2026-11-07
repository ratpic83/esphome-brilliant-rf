# esphome-brilliant-rf

Control **Brilliant RGB+CCT ceiling lamps** (2.4 GHz remote 98770/05) from Home Assistant with an
ESP32 and an nRF24L01+ module. No cloud, no app, no soldering on the lamp.

- White part: on/off, brightness, colour temperature
- RGB ring: on/off, colour, brightness
- Several lamps on one ESP (one entry per remote)
- The original remote keeps working; the ESP listens to it and stays in sync

The remote and lamps use a Powerlink PL51WT020 (PL1167/LT8900 family) radio. The packet format,
CRC and the lamp's counter rules are documented in [docs/PROTOCOL.md](docs/PROTOCOL.md). Other
lamps sold with the same LAMDEN "CCT+RGB-31Key" remote will most likely work too.

> Not affiliated with Brilliant AG or the remote's manufacturer. Protocol figured out for
> interoperability with our own lamps. Use at your own risk.

## Hardware

- ESP32 dev board (tested: AZ-Delivery ESP32 DevKitC, ESP32-D0WD-V3)
- nRF24L01+ **with PA/LNA and external antenna** on a 3.3 V adapter board (the common "nRF24
  socket adapter" with an AMS1117). Tested with an Si24R1 clone.

| Adapter | ESP32 |
|---|---|
| VCC | 5V (the adapter has its own 3.3 V regulator) |
| GND | GND |
| CE | GPIO4 |
| CSN | GPIO5 |
| SCK | GPIO18 |
| MOSI (MO) | GPIO23 |
| MISO (MI) | GPIO19 |
| IRQ | not connected |

Keep the wires short, mount the antenna before powering the module and use a power supply with at
least 1 A.

**Range:** receiving is much easier than transmitting - the PA/LNA module hears a remote two rooms
away, but the lamp's own receiver has no LNA. In our flat the ESP reached the bathroom lamp only
sporadically from the living room, and reliably from one room closer. Test transmitting, not
receiving, when you choose a place.

## Setup

1. Add the component and flash the ESP with an empty lamp list:

   ```yaml
   external_components:
     - source: github://ratpic83/esphome-brilliant-rf
       components: [brilliant_rf]

   spi:
     clk_pin: GPIO18
     miso_pin: GPIO19
     mosi_pin: GPIO23

   brilliant_rf:
     id: rf
     cs_pin: GPIO5
     ce_pin: GPIO4
   ```

2. Open the ESP log and press keys on each remote near the ESP. You get lines like

   ```
   Unknown remote, remote_id: [0x10, 0x00, 0x12, 0x34, 0x31] (channel 50, command 01)
   ```

3. Add one entry per lamp and the lights - see [examples/brilliant-rf.yaml](examples/brilliant-rf.yaml):

   ```yaml
   brilliant_rf:
     ...
     lamps:
       - remote_id: [0x10, 0x00, 0x12, 0x34, 0x31]   # lamp 0

   light:
     - platform: brilliant_rf
       lamp: 0
       type: white          # brightness + colour temperature
       name: "Ceiling white"
       default_transition_length: 0s
       gamma_correct: 1.0
       restore_mode: RESTORE_DEFAULT_OFF
     - platform: brilliant_rf
       lamp: 0
       type: rgb            # colour + brightness
       name: "Ceiling ring"
       default_transition_length: 0s
       gamma_correct: 1.0
       restore_mode: RESTORE_DEFAULT_OFF
   ```

   `default_transition_length: 0s` matters: every transition step would be a radio burst.
   `gamma_correct: 1.0` because the lamp expects linear percentages.

## Configuration

| Option | Default | |
|---|---|---|
| `cs_pin`, `ce_pin` | required | nRF24 pins |
| `channel` | 50 | RF channel (2400 + n MHz) |
| `sync_word` | `[0x8C, 0x42, 0xF3, 0x7B]` | PL1167 sync word as read MSB first |
| `lamps` | `[]` | list of `remote_id` (5 bytes); lights refer to the index |
| `repeats` | 40 | copies per packet on air |
| `log_packets` | false | log every received packet (protocol work) |
| `scan` | off | hop channels `first_channel`..`last_channel` with `dwell_time` to find a remote's channel |

Light options: `lamp` (index), `type` (`white` or `rgb`), `cold_white_color_temperature`
(6500 K), `warm_white_color_temperature` (2700 K).

## Sending other commands

The component exposes `send(lamp, command, p1, p2, p3, p4)`, e.g. as an API action:

```yaml
api:
  actions:
    - action: send_command
      variables: {lamp: int, command: int, p1: int, p2: int, p3: int, p4: int}
      then:
        - lambda: id(rf).send(lamp, command, p1, p2, p3, p4);
```

Command codes are listed in [docs/PROTOCOL.md](docs/PROTOCOL.md). `send_raw()` and
`set_counter()` exist for experiments.

## Good to know

- The lamp gives no feedback. Home Assistant shows what was sent last. If someone used the wall
  switch or the remote, the lamp may differ - for scenes, prefer a script that sends the commands
  directly.
- With the wall switch off the lamp has no power and cannot be controlled.
- ON always brings the white part back. Turning off only the white while the ring stays on is done
  with white at 0 %.
- The ESP keeps its own counter ahead of the remote and repeats remote key presses the lamp would
  otherwise drop. If the ESP is offline after a lot of Home Assistant activity, the remote may do
  nothing until its counter has caught up.
- After a reboot the ESP does not know whether the lamp is on and sends one ON before the first
  white/colour command (the white may flash briefly once).

## License

MIT, see [LICENSE](LICENSE).
