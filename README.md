# NieR-Automata-in-VR
A mod that converts the game NieR: Automata into a VR mod!

### Easy to install

Drop one file (d3d11.dll) into the game folder; no injector needed

### Easy resolution control with dlss

Automatically upscale to a percentage of your headsets native resolution using dlss, or render at native
with dlaa only

### First person mode (work in progress)

### Vr controller support with optional waggle controls (work in progress)

Get immersed in the experience. Regular controls and other controllers still work as before

### In-headset mod menu

In game mod menu for editing mod specific options 

### In game performance overlay

Shows stats in game (with a graph)

### Broad headset support (theoretically)

I've only tested on Virtual desktop with Quest 3s, so open an issue if support on another headset is broken.
The original author had tested this on many headsets and runtimes

## Install
1. Download the the latest release. Open the game folder: in Steam, right-click NieR:Automata > Manage > Browse local files

2. Unzip the release, then copy d3d11.dll inside into your NieR: Automata Steam folder next to NieRAutomata.exe

3. Launch the game, VR should work automatically as long as your headset is connected and steamvr is running like normal

4. It's highly recommended to **DISABLE BLUR** in game, it will ruin the image. And to use DLSS instead of the games own antialiasing which looks much better

5. In the game's settings, use borderless mode and pick a resolution that your computer
can comfortably run. If performance is poor, try a lower resolution and upscale with dlss

6. Open the mod menu by pressing (and holding) B then press A. Changes are automatically saved to the ini file

7. Uninstall: delete d3d11.dll (and NieRVR.ini / NieRVR*.log if you like).
[VR] Enabled=0 in NieRVR.ini turns the mod off without uninstalling it.

## Performance tips

- Alternating mode (the default) is the fastest; Synchronized (F5) looks smoother but needs about twice the GPU work
- Try DLSS to improve resolution


## VR controllers

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


## Keyboard controls

  Insert          settings menu
  F8              recenter the view and the screen in front of you
  F12             stereo 3D / flat screen
  F5              stereo mode: alternating / synchronized
  Shift+F12       steep top-down views flat on/off
  F6 / F7         screen and HUD closer / farther
  F9 / F10        screen and HUD smaller / larger
  Ctrl+F6 / F7    camera closer / farther (stronger / weaker 3D)
  Ctrl+F9 / F10   world bigger / smaller
  Ctrl+Shift+F5   head culling on/off
Diagnostics: F11 frame capture, Ctrl+F11 dump of the game's decrypted code.

## Notes

- Visuals can appear at low framerates or be jittery. Some post processing looks weird in VR (water)
- DLSS and the stereo mode (alternating / synchronized) can affect the visuals and performance greatly

## Problems?

Open an issue with NieRVR.log (the previous run's is NieRVR.prev.log), NieRVR_crash.dmp
if the game crashed, and explain what happened.
