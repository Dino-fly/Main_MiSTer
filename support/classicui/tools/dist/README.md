# Distribution files the presets name

The PVM RGB and Composite presets (chome_video.cpp `CRT_PVM_RGB`, `CRT_COMPOSITE`)
name files from the MiSTer distribution's Filters, Shadow_Masks and Gamma packs
instead of files the firmware generates. render_looks.py reads them from here
(`--dist`, default this directory) so a CRT render needs no device.

Copied byte for byte from the device's /media/fat on 2026-10-30. If a preset starts
naming another distribution file, copy it here under the same relative path; the
renderer stops and says which file is missing.

| file | md5 |
|---|---|
| Filters/Scanlines - Adaptive/SLA_Dk_030_Br_070.txt | 206779c95ebdefe13362a5fc38c612a5 |
| Filters/Upscaling - Recommended/GS_Sharpness_050.txt | 3c98df52bf72234c082392ee96723203 |
| Gamma/Pure_Gamma/gamma_110.txt | 2d85886f9db8b22a46c5e51de7b2be7c |
| Shadow_Masks/Simple (Monochrome)/Aperture Grille (No Scanlines) (1968).txt | 516c4000ecae556b9cb9b7c9a9e991b9 |
| Shadow_Masks/Simple (Monochrome)/Sony PVM (Generic) (~1980).txt | 27a2f4a76a08e40b9b54da0cf80ed9f9 |
