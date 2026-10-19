# VitAdhoc

The existing private ZIP is the earlier build-21 preview, not the newer
build-21 covers runtime now installed on the owner's Vita. Keep that newer
installation; its matching source/package review is not finished.

There is no desktop install ZIP on GitHub yet. The release download is
**source only**. These steps are for an existing local private-preview bundle;
they are not instructions to downgrade a newer installation.

Requires a homebrew-enabled Vita and VitaShell. An open GBA BIOS is included;
no proprietary BIOS or shader compiler is needed for a normal installation.

1. Open [CBOC Vita Installer](https://github.com/CBOC-Vita-Lab/vita-installer/releases) and connect your Vita.
2. Select your existing local `VitAdhoc-build21-desktop-private.zip`.
3. Optionally select your legally usable `.gba`, `.gb` and `.gbc` files or folder.
4. Choose default or custom artwork, then click **Install**.

Leave that local install ZIP zipped; the installer opens it. The source ZIP
from GitHub is not a substitute. [Manual installation](INSTALL.md) covers
VitaShell and [Building](BUILDING.md) covers the available source.

The app shows any final VitaShell action. Existing ROMs, saves and settings
are not silently replaced. A different file with the same name stops the
copy for review. Leave the experimental GPU renderer off for normal use.

Keep the computer and Vita awake through copying and checks. If disconnected,
reconnect the same Vita and choose **Resume**.

For compatible updates installed through this app, **Update app** keeps your
ROMs, saves and settings. Use the same computer and Vita, then approve the
update explicitly. Older/manual installs use **Full setup** instead.

This is a private build-21 preview. Public release remains held pending
license clarification and final package acceptance.
