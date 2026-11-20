# Build it with AI

## Part 1: For you

This repo is written so an AI assistant can build your own dial from it. It doesn't happen from a chat window, though: the AI needs to reach your computer, the dial on its USB cable, and your Home Assistant.

**What happens:**

1. **Questions first.** The AI asks what you have: the amp, the streamer, the smart plugs, Music Assistant. Nothing changes yet.
2. **A plan.** It shows you what it will add to Home Assistant and what it will build, and waits for your yes.
3. **It builds and checks.** Helpers, scripts and automations in Home Assistant, then the firmware on the dial, then a check that each part works. If you don't have something (no smart plugs, no Whisper), that part is left out rather than faked.

**What you need:**

- **[Claude Code](https://claude.com/claude-code)** on the computer the dial is plugged into. This is the route I've tested.
- **[HA-MCP](https://github.com/homeassistant-ai/ha-mcp)** connected to Claude Code, so it can read and change Home Assistant.
- **A USB-C data cable** between the computer and the dial.
- **Your Wi-Fi password and two tokens**, which you type into `config/secrets.local` yourself. The AI never types passwords or tokens.

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
- `home-assistant/helpers.yaml`: the seven helpers. **The firmware uses these exact entity ids** (`firmware/main/ha.c`).
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

1. Which amplifier? This build drives a Denon (or Marantz) AV receiver through Home Assistant's Denon AVR integration. Another amp means rewriting part 6, so say so.
2. Which streamer does Music Assistant play to, and is it wired into the amp?
3. Are the amp and the streamer powered through smart plugs Home Assistant can switch? Which?
4. Is Music Assistant installed, and which player should the dial play to?
5. Do they want voice search? It needs the Whisper add-on.
6. Do they want the music to turn itself off when the room is empty? Which area has occupancy sensors?
7. Can Home Assistant's `config` share be mounted on this computer (for updates over Wi-Fi)?
8. Will they print the wall mount?

### Step 2: Map their things to the roles

- **The Denon:** a `media_player` from the `denonavr` integration. Replaces `media_player.bathroom_denon_amp`.
- **The streamer:** a `media_player` from its own integration. Replaces `media_player.bathroom_streamer`.
- **The streamer in Music Assistant:** a `media_player` from `music_assistant`. Replaces `media_player.bathroom_streamer_ma`. Its player id goes in `MA_PLAYER`.
- **The amp's smart plug:** a `switch`. Replaces `switch.attic_storage_swb1_denon_amp`. Optional: without it, delete the power steps in `music-on-off`, and replace the timeout template in `music-amp-ready` (it reads this switch's `last_changed`) with a fixed `150`.
- **A second plug** (my sub amp's power supply): replaces `switch.attic_storage_swb2_bathroom_sub_psu`. Optional.
- **The Denon integration's config entry id:** replaces the one in `amp-connection-recovery.yaml`.
- **The area for auto-off:** replaces `bathroom` in `music-auto-off.yaml`. Optional.
- **The Denon's address:** goes in `AMP_SERVER`.

### Step 3: Show the plan and wait

List every helper, script and automation you will create, with its entity id, and every file you will change. Wait for a clear yes.

### Step 4: Home Assistant

1. Add the helpers from `helpers.yaml` with the same entity ids.
2. Install the blueprint into `blueprints/script/` and reload scripts.
3. Create the five scripts with these entity ids: `script.bathroom_amp_volume`, `script.bathroom_amp_treble`, `script.bathroom_amp_bass`, `script.bathroom_music_amp_ready`, `script.bathroom_music_fade_off`.
4. Create the automations. Give `music-on-off.yaml` the entity id `automation.bathroom_music_fade_off`, because `music-auto-off.yaml` triggers it by that name.
5. For voice search: install the Whisper add-on, accept the Wyoming integration, and check that `stt.faster_whisper` exists.

### Step 5: The firmware

1. Ask before installing ESP-IDF 5.5 into `~/esp/esp-idf`.
2. Ask the user to copy `config/secrets.example` to `config/secrets.local` and fill it in. Confirm each key has a value without printing any of them.
3. Run `tools/fw.sh build`. Then, with the dial plugged in, `tools/fw.sh flash`.

### Step 6: Check it works

- The dial starts, shows the logo, then the connecting screen ticks off Wi-Fi, Music Assistant, Home Assistant and the streamer.
- `sensor.bathroom_dial_firmware` appears in Home Assistant with the version just built.
- Turning on `input_boolean.bathroom_music` powers the amp, and `sensor.bathroom_dial_amp` goes from `connecting` to `ready`.
- Moving `input_number.bathroom_amp_volume` moves the Denon's volume.
- Playing an album from the dial plays through the amp.
- Choosing Music off on the dial fades, stops the stream and turns the helper off.
- If voice search was installed: Search, say an artist, and results appear.

### Reference: the helpers

- `input_number.bathroom_amp_volume`: 0 to 98, step 1. Required.
- `input_number.bathroom_amp_treble`: −6 to 6. Required.
- `input_number.bathroom_amp_bass`: −6 to 6. Required.
- `input_number.bathroom_amp_subwoofer`: −12 to 12. Required.
- `input_boolean.bathroom_music`: required.
- `input_boolean.bathroom_dial_update`: for updates over Wi-Fi.
- `input_text.bathroom_dial_firmware_url`: for updates over Wi-Fi.
