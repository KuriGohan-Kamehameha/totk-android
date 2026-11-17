# Research notes

Findings published as they were made, with their evidence. Code changes that came out of them are referenced by commit.

- [botw-graphics.md](botw-graphics.md): BotW on the Vulkan backend: the water colour fix, the open "onion skinning" defect
  and its suspect ([botw-rt-dup.diff](botw-rt-dup.diff), unmerged), earlier crash and start-up findings.
- [led-stamina-cutscene.md](led-stamina-cutscene.md): Bifrost's external API and the Thor's stick-LED driver (whole-ring
  only on stock firmware), where stamina lives in both games, and the cutscene/event signal both second screens use.
- [thor-per-led.md](thor-per-led.md): per-LED control of the Thor's stick rings: the chip can do it (12 PWM channels per
  stick), the stock driver only paints whole rings, no unrooted path exists; the recommended root route and its risks.
  It corrects the bus numbers in led-stamina-cutscene.md (left ring i2c-3, right ring i2c-5).
- [bifrost-api-v2.md](bifrost-api-v2.md): a Bifrost change (on the P1R4N351 fork) letting the bridge run presets and reactive modes
  on demand, and an existing Bifrost bug: it cannot tell external callers apart.
- [../companion-waist.md](../companion-waist.md): every value, action and service the two second screens use, and how
  each maps to one shared layer.
