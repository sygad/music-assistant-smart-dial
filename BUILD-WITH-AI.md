# Build it with AI

## Part 1: For you

This repo is written so an AI assistant can build your own dial from it. It doesn't happen from a chat window, though: the AI needs to reach your computer, the dial on its USB cable, and your Home Assistant.

**What happens:**

1. **Questions first.** The AI asks what you have: Music Assistant, and whether there's an amp, a streamer and smart plugs. Nothing changes yet. **You don't need any of my equipment.** With just the dial, Home Assistant and Music Assistant, you get the whole dial on screen: browse, Now Playing, the volume arc and the tone controls. **You won't hear any music** until there's a streamer to play to, and the volume and tone change nothing until there's an amp. Add them later and they start working.
2. **A plan.** It shows you what it will add to Home Assistant and what it will build, and waits for your yes.
3. **It builds and checks.** Helpers, scripts and automations in Home Assistant, then the firmware on the dial, then a check that each part works. If you don't have something (no smart plugs, no Whisper), that part is left out rather than faked.

**What you need:**

- **[Claude Code](https://claude.com/claude-code)** on the computer the dial is plugged into. This is the route I've tested.
- **[HA-MCP](https://github.com/homeassistant-ai/ha-mcp)** connected to Claude Code, so it can read and change Home Assistant.
- **A USB-C data cable** between the computer and the dial.
- **Your Wi-Fi password and two tokens**, which you type into `config/secrets.local` yourself: a Home Assistant long-lived access token (in Home Assistant, your profile → Security) and a Music Assistant long-lived token (in Music Assistant, Settings → Profile). The AI never types passwords or tokens.

Other AI assistants with a similar connection to your computer and Home Assistant should work too. Without a connection, an AI can still talk you through it, and you do the typing.

**The prompt:**

```text
I want to build my own Music Assistant Smart Dial from
https://github.com/sygad/music-assistant-smart-dial

Read BUILD-WITH-AI.md in that repo first and follow its "Part 2: For the AI"
section exactly. Start with the questions in Step 1, one at a time.
Don't install, create or change anything until I've approved your plan.
```

---

## Part 2: For the AI

### The files

- `README.md`: the overview and the parts list.
- `parts/NN-*/README.md`: one page per part. Read the page for a part before building it.
- `home-assistant/helpers.yaml`: the seven helpers. The firmware names every helper and sensor from `DIAL_ROOM` in `config/secrets.local`: `your_room` gives `input_number.your_room_amp_volume`. Every file in `home-assistant/` is written for `your_room`. If the user picks another room name, change `your_room_` to theirs in every file you copy, and use the same name for `DIAL_ROOM`.
- `home-assistant/blueprints/denon_avr_audio_control.yaml`: a script blueprint.
- `home-assistant/scripts/*.yaml`, `home-assistant/automations/*.yaml`: the Home Assistant side. Each has a header saying what it is, and `# CHANGE:` on every value that is specific to my house.
- `config/secrets.example`: every setting the firmware needs. The user copies it to `config/secrets.local`.
- `tools/fw.sh`: builds (`build`), flashes over USB (`flash`), updates over Wi-Fi (`ota`).
- `core/`, `ui/`, `firmware/`: the source. Don't change it to get a build through; stop and tell the user.

### Rules

- Never change or delete the user's existing Home Assistant config. Add alongside it.
- Ask before installing anything (ESP-IDF, Emscripten, Python packages, add-ons, integrations).
- Never guess an entity id. Check it exists in Home Assistant before you write it into a file.
- Copy the YAML as it is. Change only the lines marked `CHANGE`, and the entity ids listed in the header of each file.
- Never type a password or token. Ask the user to put them in `config/secrets.local` themselves, and never print that file.
- If the user doesn't have something a part needs, leave that part out, and anything only it uses. Never fake a value.
- One question at a time.

### Step 1: Ask the user

1. Is Music Assistant installed? Is there a streamer for the dial to play to, and which Music Assistant player is it? **"No streamer" is a full answer:** leave `MA_PLAYER` blank. The dial says "No streamer" when it starts, then browses as usual, and nothing plays. Tell the user they won't hear music.
2. What room is the dial for? This becomes `DIAL_ROOM`, lower case with `_` for spaces.
3. Is there an amplifier, and which? Part 6 drives a Denon or Marantz AV receiver through Home Assistant's Denon AVR integration. **"No amp" is a full answer:** leave `AMP_SERVER` blank, and the dial treats the amp as always connected. Every screen works, and the volume and tone controls change nothing. Build that, and skip the amp questions below. Another make of amp means rewriting part 6, so say so and offer the no-amp build as a first step.
4. If there's an amp: which streamer does Music Assistant play to, and is it wired into the amp?
5. If there's an amp: are the amp and the streamer powered through smart plugs Home Assistant can switch? Which?
6. Do they want voice search? It needs the Whisper add-on.
7. Do they want the music to turn itself off when the room is empty? Which area has occupancy sensors?
8. Can Home Assistant's `config` share be mounted on this computer (for updates over Wi-Fi)?
9. Will they print the wall mount?

### With no amp

Make the helpers (Step 4, item 1) and build the firmware (Step 5) with `AMP_SERVER` blank. Leave out the blueprint, the scripts and every automation except `music-auto-off` if they want it: all of them exist to drive the amp. Music off then turns the music helper off but doesn't stop Music Assistant playing, because stopping the stream is in `music-on-off`, which also powers the amps down. Tell the user that, and that the rest of part 6 can be added once they have an amp.

### Step 2: Map their things to the roles

- **The Denon:** a `media_player` from the `denonavr` integration. Replaces `media_player.your_room_denon_amp`.
- **The streamer:** a `media_player` from its own integration. Replaces `media_player.your_room_streamer`.
- **The streamer in Music Assistant:** a `media_player` from `music_assistant`. Replaces `media_player.your_room_streamer_ma`. Its player id goes in `MA_PLAYER`.
- **The amp's smart plug:** a `switch`. Replaces `switch.attic_storage_swb1_denon_amp`. Optional: without it, delete the power steps in `music-on-off`, and replace the timeout template in `music-amp-ready` (it reads this switch's `last_changed`) with a fixed `150`.
- **A second plug** (my sub amp's power supply): replaces `switch.attic_storage_swb2_bathroom_sub_psu`. Optional.
- **The Denon integration's config entry id:** replaces the one in `amp-connection-recovery.yaml`.
- **The area for auto-off:** replaces `bathroom` in `music-auto-off.yaml`. Optional.
- **The Denon's address:** goes in `AMP_SERVER`. Blank if there's no amp.

### Step 3: Show the plan and wait

List every helper, script and automation you will create, with its entity id, and every file you will change. Wait for a clear yes.

### Step 4: Home Assistant

1. Add the helpers from `helpers.yaml` with the same entity ids.
2. Install the blueprint into `blueprints/script/` and reload scripts.
3. Create the five scripts with these entity ids: `script.your_room_amp_volume`, `script.your_room_amp_treble`, `script.your_room_amp_bass`, `script.your_room_music_amp_ready`, `script.your_room_music_fade_off`.
4. Create the automations. Give `music-on-off.yaml` the entity id `automation.your_room_music_fade_off`, because `music-auto-off.yaml` triggers it by that name.
5. For voice search: install the Whisper add-on, accept the Wyoming integration, and check that `stt.faster_whisper` exists.

### Step 5: The firmware

1. Ask before installing ESP-IDF 5.5 into `~/esp/esp-idf`.
2. Ask the user to copy `config/secrets.example` to `config/secrets.local` and fill it in. Confirm each key has a value without printing any of them.
3. Run `tools/fw.sh build`. Then, with the dial plugged in, `tools/fw.sh flash`.

### Step 6: Check it works

With no amp: the dial starts, connects, opens the menu, plays an album (with no streamer it says "No streamer" and nothing plays), and the volume arc moves. Then `sensor.your_room_dial_firmware` appears in Home Assistant. That's the whole check. With an amp:

- The dial starts, shows the logo, then the connecting screen ticks off Wi-Fi, Music Assistant, Home Assistant and the streamer.
- `sensor.your_room_dial_firmware` appears in Home Assistant with the version just built.
- Turning on `input_boolean.your_room_music` powers the amp, and `sensor.your_room_dial_amp` goes from `connecting` to `ready`.
- Moving `input_number.your_room_amp_volume` moves the Denon's volume.
- Playing an album from the dial plays through the amp.
- Choosing Music off on the dial fades, stops the stream and turns the helper off.
- If voice search was installed: Search, say an artist, and results appear.

### Reference: the helpers

- `input_number.your_room_amp_volume`: 0 to 98, step 1. Required.
- `input_number.your_room_amp_treble`: −6 to 6. Required.
- `input_number.your_room_amp_bass`: −6 to 6. Required.
- `input_number.your_room_amp_subwoofer`: −12 to 12. Required.
- `input_boolean.your_room_music`: required.
- `input_boolean.your_room_dial_update`: for updates over Wi-Fi.
- `input_text.your_room_dial_firmware_url`: for updates over Wi-Fi.
