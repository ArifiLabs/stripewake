# Stripewake privacy policy

Last updated: 2026-10-17

**Stripewake collects no personal information and sends no data. It makes no network connections.**

- **No account, no telemetry, no ads, no analytics.** The program has no network code at all.
- **What it keeps, only on your PC:**
  - its settings (your menu choices) in the Windows registry under `HKCU\Software\Stripewake`;
  - `restore.ini`: the original power-plan values it must put back when it turns OFF;
  - `dim.ini`: a monitor's original brightness, only while Dim when away has it dimmed;
  - `state.log`: one dated line per change it makes, so you can see what it did.
- **Where:** the desktop installer version keeps the files in `%LOCALAPPDATA%\Stripewake`. The Microsoft Store version
  keeps them in its own app folder, and Windows removes them when you uninstall it.
- **Nothing leaves your PC.** None of this is sent to Arifi Labs or anyone else.

Questions: open an issue at <https://github.com/ArifiLabs/stripewake/issues>.
