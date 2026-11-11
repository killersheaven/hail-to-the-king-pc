Evil Dead: Hail to the King - unofficial Android build (early test)
(c) 2026 Killers' Heaven - free software under the GNU GPL v3

This app was built on your PC from your own disc. It needs:
  - a 64-bit Android phone or handheld, Android 7 or newer (12+ recommended)
  - a game controller (Bluetooth or USB: Xbox, PlayStation, Switch Pro, or a
    handheld's built-in controls). There are no touch controls yet.
  - about 1 GB free for the disc image

1. INSTALL THE APP
   Copy hail-to-the-king.apk to your phone (USB cable, cloud drive, email...),
   tap it in the Files app, and allow "install unknown apps" for that app when
   Android asks. (Or, with USB debugging on: adb install hail-to-the-king.apk)

2. START IT ONCE
   It shows a message with the folder where your disc goes, and closes.
   (This creates the folder.)

3. COPY YOUR DISC TO THE PHONE
   Connect the phone to your PC with USB and choose "File transfer". On the PC,
   open the phone's storage, then:
       Android > data > com.killersheaven.hailtotheking > files
   and copy in your disc image: the .cue file and every .bin file that belongs
   to it (about 1 GB). Keep them together in that folder (or in a "disc"
   folder inside it).
   (Android's own Files app can't open Android/data on most phones; a PC can.)

4. PLAY
   Start the app. The launcher opens: Play, Options, Extras. Use the
   controller (d-pad / stick to move, A to choose, B to go back). The phone's
   Back button goes back in the launcher and quits from the game.

YOUR FILES
   Saves (vmu_a1.bin, vmu_a2.bin), settings.ini and log.txt live in the same
   folder as the disc. If something goes wrong, copy log.txt to your PC and
   send it with your report.

UPDATING
   Build the new version on your PC and install the new .apk over the old one.
   Your saves and the disc stay where they are. (The PC build signs the app
   with a key kept in its .tools folder; keep that folder, or Android will
   refuse the update until you uninstall the old app - which deletes the
   folder above, saves included, so copy them off first.)

Please don't share the .apk: it contains code translated from the game.
