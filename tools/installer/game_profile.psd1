# What this port needs from the player's game copy. Read by tools\installer\gamefiles.ps1
# (used by setup.ps1 and the installer). Paths are relative to the folder that contains PS3_GAME.
@{
    GameName    = 'Resistance 3'
    TitleId     = 'BCUS98176'
    RegionLabel = 'US'
    AppVer      = '01.00'
    TitleMatch  = 'Resistance.?3'                 # other regions of the same game (for a clearer message)
    Required    = @('PS3_GAME/PARAM.SFO', 'PS3_GAME/USRDIR/EBOOT.BIN', 'PS3_GAME/USRDIR/packed/game/game.psarc')
    Program     = @('PS3_GAME/USRDIR/EBOOT.BIN')   # the PS3 program file Setup decrypts
    Sha1        = @{
        'PS3_GAME/USRDIR/EBOOT.BIN' = '43bad7101e8e54b1f37f65674affef59bfb2ff53'
    }
    # Other discs of the same game that are accepted too (title id = region label). Their EBOOT.BIN hash is not
    # known, so only the decrypted program is checked (setup.ps1, Eboots below).
    AltTitleIds = @{ 'BCES01118' = 'experimental, not yet tested on a real copy' }
    AltLabel    = 'or the European disc BCES01118, experimental'
    # The decrypted programs (RPCS3's PPU hash of EBOOT.elf, as in its patch.yml) and what Setup does with them:
    # ok = supported, experimental = accepted but untested, update = a game update (not supported).
    Eboots      = @(
        @{ Ppu = '502feb2351d2b637a2704d5a50cb44bc1b781532'; Id = 'BCUS98176'; Ver = '01.00'; Region = 'US'; Status = 'ok' }
        @{ Ppu = '2701cf372814d59f26e771dc9394323d4f799682'; Id = 'BCES01118'; Ver = '01.00'; Region = 'Europe'; Status = 'experimental' }
        @{ Ppu = '4aa1887bfdd285e3026e6a97d65ed617ddcf4789'; Id = 'BCUS98176'; Ver = '01.05'; Region = 'US'; Status = 'update' }
        @{ Ppu = 'cd72aa19da7e24a00802932d06460034c7d94fc6'; Id = 'BCES01118'; Ver = '01.05'; Region = 'Europe'; Status = 'update' }
    )
    MinBytes    = 33500000000
    SizeLabel   = '34.6 GB'
    BuildBytes  = 12000000000                    # RPCS3 source, generated code, the build and the shader cache
    SaveFolder  = 'Resistance 3'                 # %USERPROFILE%\Saved Games\<this>
}