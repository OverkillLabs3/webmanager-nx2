# Installing the web file manager on the Switch 2

You need a Nintendo Switch 2 running kaiHEN, a PC to put the file on the SD card (or an FTP connection to the console), and a browser on the same network.

## 1. Put the plugin on the SD card

1. Download `webmanager-nx2-1.0.0.zip` from the Releases page.
2. Unzip it onto the **root** of the console's SD card. If asked, allow it to merge with the existing `kaihen` folder. You should end up with:

```
SD card
└── kaihen
    └── plugins
        └── webfm.nso
```

Other plugins in `kaihen/plugins/` are not touched.

## 2. Restart

Restart the console and start kaiHEN as usual. Plugins start together with kaiHEN, before you open the homebrew menu.

To check that it started, open `/kaihen/plugin-status.txt` on the card. It has one line per plugin, and yours should read `webfm: running`.

## 3. Open it

Find the console's IP address in the console's network settings, then open this in a browser on the same network:

```
http://<console IP>:8888
```

The page shows the SD card. Drag files or folders onto it to upload.

If you cannot connect: check that the console and the PC are on the same network, that nothing else uses port 8888, and that `plugin-status.txt` lists `webfm`. Use the IP address, not a host name: the plugin refuses host names.

## Removing it

Delete `/kaihen/plugins/webfm.nso` from the card and restart. If the console ever fails to start kaiHEN after installing a plugin, take the card out and delete the plugin file the same way.
