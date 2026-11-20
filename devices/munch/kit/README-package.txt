iOS 16.5 on the POCO F4 (munch), in RAM

  1. Connect the phone in Android (USB debugging on) or in fastboot (Power + Volume Down).
  2. Run start.bat (with Sileo in the package it first asks whether to boot with it). It boots Mu-Silicium and the loader with "fastboot boot", uploads system.apfs (about 100 s)
     and starts the kernel. iOS appears about 30-45 s later.
  3. A reboot (hold Power) brings Android back. Nothing is ever flashed or written to the phone's storage.

Logs: get-log.bat saves the phone's log into logs\ (while iOS runs, or after it returned to the loader;
not after a reboot to Android). The Wi-Fi address and serial number are blanked out in it.

This folder holds Apple's files (from the IPSW), your phone's firmware and your Wi-Fi password: keep it to yourself.
