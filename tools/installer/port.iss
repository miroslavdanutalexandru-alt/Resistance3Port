; Installer for the Resistance 3 native PC port.
; Build it with:  powershell -ExecutionPolicy Bypass -File tools\installer\build_installer.ps1
; The installer holds only the port's own files; the player's game copy is
; located on first run and prepared by Setup.bat.

#define PortName      "Resistance 3 PC Port"
#define PortShort     "Resistance3Port"
; PortLinkName: the Start Menu shortcut. RecompLauncher matches an installed game by two words of its
; name ("resistance" and "resistance3" from its catalog/repo name); "Resistance 3 PC Port" alone has one.
#define PortLinkName  "Resistance 3 PC Port (Resistance3Port)"
#define PortAppId     "{{6C1E6A0B-2F4D-4C55-9A4E-1F0A9B3D7E03}"
#define PortUrl       "https://github.com/miroslavdanutalexandru-alt/Resistance3Port"
#define GameTitle     "Resistance 3 (US disc BCUS98176, or EU disc BCES01118 - experimental)"
; UserDir: the folder under %USERPROFILE%\Saved Games for the settings backup
#define UserDir       "Resistance 3"

#include "port_common.iss"
