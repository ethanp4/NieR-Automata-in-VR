# NieR-Automata-in-VR
A mod that converts the game NieR: Automata into a VR mod!

### Easy to install

Drop one file (d3d11.dll) into the game folder; no injector needed

### Resolution optimization with dlss included

Automatically upscale to a percentage of your headsets native resolution using dlss, or render at native
with DLAA only

### First person mode:

work in prorgess

### Vr controller support with (optional) waggle controls:

get immersed in the experience

### In-headset mod menu

In game mod menu for editing mod specific options 

### In game performance overlay:

Work in progress

### Broad headset support (theoretically):

I've only tested on Virtual desktop with Quest 3s, so open an issue if support on another headset is broken.
The original author had tested this on many headsets and runtimes

## Install
1. Download the the latest release (Not the source files). Open the game folder: in Steam, right-click NieR:Automata > Manage > Browse local files. You should see NieRAutomata.exe.

2. Unzip the release, then copy d3d11.dll inside into your NieR: Automata Steam folder.
Start your VR runtime (SteamVR) first, then the game from Steam.

3. It's highly recommended to **DISABLE BLUR** in game, it will ruin the image. And to use DLSS instead of the games own antialiasing which looks much better

4. In the game's settings, use borderless mode and pick a resolution that your computer
can comfortably run. VR can be quite demanding, so try using DLSS if performance is poor

5. Open the mod menu by pressing (and holding) B then press A. Changes are automatically saved

6. Uninstall: delete d3d11.dll (and NieRVR.ini / NieRVR*.log if you like).
[VR] Enabled=0 in NieRVR.ini turns the mod off without uninstalling it.

## Performance tips

- Alternating eyes will affect performance greatly, but may cause eye strain
or jitteryness
- Rendering page: Global illumination volumes 16-32 (about 1 ms per frame
  at 4.8K), Ambient occlusion off (about +1.5 fps), Shadows off.
- Frame queue 1 (default) gives the lowest delay; 2 may run slightly faster.
- Try DLSS to improve resolution


## VR controllers

On by default (menu > Controls). A real gamepad and keyboard/mouse keep
working at the same time.

    Quest / Pico / Index          Game (Xbox gamepad)
    --------------------          -------------------
    left stick, right stick       left stick, right stick
    left / right trigger          LT / RT
    left / right grip             LB / RB
    A, B (right hand)             A, B
    X, Y (left hand)              X, Y   (Index: left A, B)
    stick clicks                  L3, R3
    left menu button, tap         Start
    left menu button, hold        Back (View)
    left menu button + left stick D-pad (keep the menu button held)
    (Index: press the left trackpad firmly for the menu button)

Vive wands have few buttons: right trackpad click = A, right menu = B,
trackpads = sticks.


## Keys (while the game window has focus)

  Insert          settings menu ([Keys] Menu)
  F8              recenter the view and the screen in front of you
  F12             stereo 3D / flat screen
  F5              both eyes every frame on/off
  Shift+F12       steep top-down views flat on/off
  F6 / F7         screen and HUD closer / farther
  F9 / F10        screen and HUD smaller / larger
  Ctrl+F6 / F7    camera closer / farther (stronger / weaker 3D)
  Ctrl+F9 / F10   world bigger / smaller
  Ctrl+F11        ambient occlusion on/off
  Ctrl+Shift+F11  shadows on/off
  Ctrl+Shift+F5   head culling on/off
Diagnostics: F11 capture, Ctrl+F12 single-eye test, Ctrl+F5 camera watch,
Shift+F5 culling test steps.

## Notes

- Visuals can appear at low framerates or be jittery. Some post processing looks weird in VR
- DLSS and Alternating eyes can affect the visuals greatly.

## Problems?

Open an issue with NieRVR.log (the previous run's is NieRVR.prev.log), NieRVR_crash.dmp
if the game crashed, and explain what happened.
