# PC-port milestones

## 1. Repeatable stock-game route

Build the pinned runtime, regenerated cartridge code, and generated BIOS. Verify cold boot, language selection, title, character selection, and the first playable map.
Create an input replay that demonstrates movement, attacking, receiving damage, one skill, pause/resume, room transition, and EEPROM save/reload.
Retain screenshots and coverage per checkpoint. Compare the same route in mGBA; matching the framework's own interpreter alone does not prove its hardware model is correct.

## 2. Execution coverage

Investigate each missing IWRAM/ROM entry and record whether it is an indirect call, an interior resume, or modified RAM code.
Fix discovery or runtime support at the appropriate source layer. Review metadata changes instead of copying miss proposals blindly or editing generated C++.
Run strict native checkpoints separately. A cached runtime recompilation is useful progress but remains distinct from ahead-of-time coverage.

## 3. Expanded compatibility

Add the tested Expanded ROM as a separately verified target. Import symbols from its linked ELF, including code outside the original cartridge's 16 MiB.
Validate the mod's startup hooks, extra RAM reservations, engine tables, saves, characters, Gauntlet scenes, and custom effect data.
Keep stock and Expanded configurations and saves separate.

## 4. Replace one engine service

Use the original game as the behavior reference. Start with a contained service and keep a faithful build available for comparison.
Potential experiments include a host-rendered HUD or an independently validated native graphics upload path. Measure the result before expanding the scope.
Replacing the actor allocator or actor loop needs guest-pointer compatibility, collision/targeting/lifetime checks, and a plan for fixed-size actor tables. More host memory alone cannot extend those structures.
Larger sprite/effect capacity needs a renderer and submission path that can represent it; changing the CPU's execution backend does not change GBA OAM or tile memory.

## 5. PC enhancements

Once a playable route and service replacement are qualified, investigate larger actor pools, heavier spell effects, wider views, more flexible interfaces, and host storage.
Keep changes to gameplay timing and simulation explicit, with reproducible fight scenarios and saved-state checks. These enhancements are not claimed by the initial boot experiment.
