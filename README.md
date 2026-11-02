# WorkshopDL++

The Ultimate Steam Workshop downloader.

**Download:** [Latest release](https://github.com/noitavo/workshopdl/releases/latest) (Windows 10/11 x64)

- Everything you would expect from the legacy Clickteam Fusion 2.5 WorkshopDL.
- Browser for Workshop items and collections.
- Mod manager for supported games.

Some downloads require a Steam account that owns the game.<br>
WorkshopDL is self contained. It stores its data beside the executable, so extract the release to a writable folder.

Copyright (c) 2022-2026 noitavo. [License](LICENSE): CC BY-NC-SA 4.0.

<img width="1920" height="1040" alt="Workshop browser" src="https://github.com/user-attachments/assets/b0d999df-bdfb-4253-9b18-c2c0762fd357" />

## Requirements

- Windows 10 version 1809 or later, or Windows 11 (x64).
- [Microsoft Edge WebView2 Runtime](https://developer.microsoft.com/microsoft-edge/webview2/) for the Browse tab. It is included with Windows 11. The Downloads tab works without it.
- Internet access. SteamCMD is downloaded from Valve on first launch.

## Usage

1. Extract the release to a writable folder and run `WorkshopDL.exe`.
2. Open a Workshop item or collection in **Browse**, or paste links into **Downloads**.
3. Choose **Download now**, or **Add to queue** and start the queue later.

Links can also be passed on the command line: `WorkshopDL.exe <link or item ID>`.

## Mods tab

| Support | What it does |
| --- | --- |
| Full | Install, enable and order mods from the app. |
| Install | Install mods here, then enable them in the game. |
| Download only | Download and open the folder. Use **Folders > Set mod folder** to add an install rule. |

Profiles save which mods are enabled for a game, and can be exported as a list of Workshop links that downloads whatever is missing when imported. RimWorld also gets automatic load-order sorting and dependency warnings.

## Troubleshooting

| Problem | What to do |
| --- | --- |
| Download fails when not signed in | Sign in with an account that owns the game. |
| SteamCMD setup fails | Check the connection and security software, then use **Settings > Maintenance > Repair SteamCMD**. |
| Browse tab is empty | Install the WebView2 Runtime. |
| Item needs a different game | Right-click it in the queue and choose **Set game App ID**. |
| Reporting a bug | Attach the export from **Downloads > History > Export log**. Crash reports are saved in `data\crashes`. |

---
> [!WARNING]
> This and the [nexusmods](https://www.nexusmods.com/site/mods/2493) page are the **only official sources** for **WorkshopDL**.
>
> Any third-party websites, repositories, or platforms claiming to distribute WorkshopDL are **unauthorized and are not affiliated with this project**. Such sources may distribute modified, malicious, or otherwise unsafe versions of the software.
>
> **For your security, only download WorkshopDL from this official GitHub repository.** Avoid unofficial download links, mirrors, and websites claiming to represent the project.

---
Thanks to [imwaitingnow](https://github.com/imwaitingnow/WorkshopDL) for maintaining WorkshopDL after my original repository and account were deleted.

<b>This project is not affiliated with Valve.</b>
